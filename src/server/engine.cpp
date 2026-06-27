#include "opaquedb/server/engine.h"

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "opaquedb/backend/pir_backend.h"
#include "opaquedb/core/key_codec.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/core/slot_codec.h"
#include "opaquedb/core/wire.h"
#include "opaquedb/crypto/serialize.h"
#include "opaquedb/server/ingest.h"
#include "opaquedb/sql/logical_plan.h"
#include "opaquedb/sql/parser.h"

namespace opaquedb::server {
namespace {

// Builds the key column (one key value per row) and the payload column (raw
// record bytes per row) the backend evaluates over, reading every row of the
// epoch. The backend lays both out into SIMD slots itself.
//
// A match record packs one key per searchable column. match_offset is the byte
// offset of the queried column's sub-key inside that record (its SearchableRank
// times the per-key stride), so a query on a secondary index reads its own
// sub-key while the matcher stays single-key.
absl::Status BuildColumns(const storage::EpochReader &reader,
                          std::size_t match_offset, backend::KeyColumn *keys,
                          backend::PayloadColumns *payload) {
  const storage::EpochManifest &manifest = reader.manifest();
  keys->key_bits = manifest.key_bits;
  payload->record_bytes = manifest.geometry.record_bytes;
  payload->bytes_per_slot = manifest.geometry.bytes_per_slot;

  const std::size_t stride = storage::MatchRecordBytes(manifest.key_bits);
  const std::uint64_t rows = reader.row_count();
  keys->keys.reserve(rows);
  payload->rows.reserve(rows);
  for (std::uint64_t r = 0; r < rows; ++r) {
    absl::StatusOr<std::span<const std::uint8_t>> match_bytes =
        reader.MatchRecord(r);
    if (!match_bytes.ok())
      return match_bytes.status();
    if (match_offset + stride > match_bytes->size()) {
      return absl::DataLossError(
          "match record is too short for the queried column's sub-key");
    }
    absl::StatusOr<std::uint64_t> value = core::UnpackKey(
        match_bytes->subspan(match_offset, stride), manifest.key_bits);
    if (!value.ok())
      return value.status();
    keys->keys.push_back(*value);

    absl::StatusOr<std::span<const std::uint8_t>> payload_bytes =
        reader.PayloadRecord(r);
    if (!payload_bytes.ok())
      return payload_bytes.status();
    payload->rows.emplace_back(payload_bytes->begin(), payload_bytes->end());
  }
  return absl::OkStatus();
}

} // namespace

absl::StatusOr<std::unique_ptr<Engine>>
Engine::Create(const config::CryptoConfig &crypto_cfg,
               storage::EpochRepository *repo, admin::KeyringStore *keyring,
               const backend::BackendRegistry &registry) {
  if (repo == nullptr) {
    return absl::InvalidArgumentError("engine: repo is required");
  }
  // A single-table engine resolves every table to the one repository.
  return Create(
      crypto_cfg,
      [repo](const core::TableId &)
          -> absl::StatusOr<storage::EpochRepository *> { return repo; },
      keyring, registry);
}

absl::StatusOr<std::unique_ptr<Engine>>
Engine::Create(const config::CryptoConfig &crypto_cfg,
               RepositoryResolver resolver, admin::KeyringStore *keyring,
               const backend::BackendRegistry &registry) {
  if (resolver == nullptr || keyring == nullptr) {
    return absl::InvalidArgumentError(
        "engine: repository resolver and keyring are required");
  }
  absl::StatusOr<crypto::CryptoContext> ctx =
      crypto::CryptoContext::Create(crypto_cfg);
  if (!ctx.ok())
    return ctx.status();
  return std::unique_ptr<Engine>(
      new Engine(*std::move(ctx), std::move(resolver), keyring, registry,
                 crypto_cfg.result_buckets));
}

absl::Status Engine::RegisterClient(const std::string &client_id,
                                    const crypto::KeyMaterial &keys) {
  if (client_id.empty()) {
    return absl::InvalidArgumentError("client id is empty");
  }
  // Validate the keys load against the context before storing them, so a bad
  // upload is rejected at registration rather than at query time.
  absl::StatusOr<crypto::EvalKeys> loaded = crypto::LoadKeyMaterial(ctx_, keys);
  if (!loaded.ok())
    return loaded.status();
  // Evict any cached evaluation contexts for this client; its keys changed.
  {
    std::lock_guard<std::mutex> lock(eval_cache_mu_);
    const std::string prefix = client_id + '\0';
    for (auto it = eval_cache_.begin(); it != eval_cache_.end();) {
      if (it->first.compare(0, prefix.size(), prefix) == 0)
        it = eval_cache_.erase(it);
      else
        ++it;
    }
  }
  return keyring_->Put(client_id, keys);
}

absl::StatusOr<std::shared_ptr<backend::EvalContext>>
Engine::EvalContextFor(const std::string &client_id,
                       const std::string &backend_name,
                       backend::PirBackend &backend) {
  const std::string key = client_id + '\0' + backend_name;
  {
    std::lock_guard<std::mutex> lock(eval_cache_mu_);
    auto it = eval_cache_.find(key);
    if (it != eval_cache_.end())
      return it->second;
  }
  absl::StatusOr<crypto::KeyMaterial> keys = keyring_->Get(client_id);
  if (!keys.ok()) {
    return absl::UnauthenticatedError(
        absl::StrCat("client '", client_id, "' has not registered keys"));
  }
  absl::StatusOr<std::unique_ptr<backend::EvalContext>> eval =
      backend.load_keys(*keys);
  if (!eval.ok())
    return eval.status();
  std::shared_ptr<backend::EvalContext> shared(*std::move(eval));
  {
    std::lock_guard<std::mutex> lock(eval_cache_mu_);
    eval_cache_[key] = shared;
  }
  return shared;
}

absl::StatusOr<core::Schema> Engine::DescribeTable(const std::string &database,
                                                   const std::string &table) {
  core::TableId id{
      database.empty() ? std::string(core::kDefaultDatabase) : database, table};
  absl::StatusOr<storage::EpochRepository *> repo = resolve_repo_(id);
  if (!repo.ok())
    return repo.status();
  absl::StatusOr<std::unique_ptr<storage::EpochReader>> reader =
      (*repo)->OpenCurrent();
  if (!reader.ok())
    return reader.status();
  return (*reader)->manifest().schema;
}

absl::StatusOr<Engine::InsertOutcome>
Engine::InsertRow(const std::string &database, const std::string &table,
                  const std::vector<std::string> &cells) {
  if (table.empty()) {
    return absl::InvalidArgumentError("insert: table is required");
  }
  core::TableId id{
      database.empty() ? std::string(core::kDefaultDatabase) : database, table};
  absl::StatusOr<storage::EpochRepository *> repo = resolve_repo_(id);
  if (!repo.ok())
    return repo.status();
  absl::StatusOr<InsertResult> result = InsertRowAndPublish(**repo, cells);
  if (!result.ok())
    return result.status();
  return InsertOutcome{result->epoch_version, result->row_count};
}

absl::StatusOr<Engine::ShardResult> Engine::EvaluateShard(
    const std::string &client_id, const std::string &database,
    const std::string &sql_template, const std::string &encrypted_param,
    const std::string &backend_hint, std::uint64_t epoch_version) {
  if (client_id.empty()) {
    return absl::InvalidArgumentError("client id is empty");
  }

  // Parse and build the logical plan from the public template.
  absl::StatusOr<sql::SelectStatement> statement = sql::Parse(sql_template);
  if (!statement.ok())
    return statement.status();
  absl::StatusOr<sql::LogicalPlan> logical = sql::BuildLogicalPlan(*statement);
  if (!logical.ok())
    return logical.status();

  // Route to the repository that holds this table. The table comes from the
  // SQL; the database is the request's, defaulting to "default".
  core::TableId id{database.empty() ? std::string(core::kDefaultDatabase)
                                    : database,
                   logical->table};
  absl::StatusOr<storage::EpochRepository *> repo = resolve_repo_(id);
  if (!repo.ok())
    return repo.status();

  // Pin this node's current epoch for the whole evaluation.
  absl::StatusOr<std::unique_ptr<storage::EpochReader>> reader =
      (*repo)->OpenCurrent();
  if (!reader.ok())
    return reader.status();
  const storage::EpochManifest &manifest = (*reader)->manifest();

  // Enforce snapshot consistency across the cluster: a coordinator pins one
  // epoch and every shard must answer from it.
  if (epoch_version != 0 && manifest.epoch_version != epoch_version) {
    return absl::FailedPreconditionError(
        absl::StrCat("shard is at epoch ", manifest.epoch_version,
                     " but the query pinned ", epoch_version));
  }

  absl::StatusOr<planner::PhysicalPlan> plan =
      planner_.Plan(manifest.schema, *logical, backend_hint);
  if (!plan.ok())
    return plan.status();

  // The matcher returns payload planes, which hold every column except the
  // primary key (kEq). Secondary indexes (kIndex) are payload, so they are
  // projectable; the primary key is not.
  for (std::size_t idx : plan->projection_indices) {
    if (manifest.schema.columns()[idx].encoding == core::ColumnEncoding::kEq) {
      return absl::UnimplementedError(
          "the primary key column is not projectable; it is matched, not "
          "returned");
    }
  }

  absl::StatusOr<std::unique_ptr<backend::PirBackend>> pir =
      registry_.create(plan->backend_name, ctx_);
  if (!pir.ok())
    return pir.status();
  absl::StatusOr<std::shared_ptr<backend::EvalContext>> eval =
      EvalContextFor(client_id, plan->backend_name, **pir);
  if (!eval.ok())
    return eval.status();

  // Deserialize and validate the encrypted operand(s) against the context. Each
  // is a fresh top-level encryption of one lookup value, bit-expanded and tiled
  // across slots. A point query carries one; IN / same-column OR carries one per
  // listed value. The plan fixes how many to expect (from the public template),
  // capped so a hostile template cannot drive unbounded FHE work.
  constexpr std::size_t kMaxOperands = 64;
  if (plan->match_operands == 0 || plan->match_operands > kMaxOperands) {
    return absl::InvalidArgumentError(
        absl::StrCat("query has ", plan->match_operands,
                     " match operands, supported range is [1, ", kMaxOperands,
                     "]"));
  }
  absl::StatusOr<std::vector<seal::Ciphertext>> operands =
      crypto::DeserializeCiphertexts(
          ctx_, encrypted_param,
          /*max_count=*/static_cast<std::uint32_t>(plan->match_operands));
  if (!operands.ok())
    return operands.status();
  if (operands->size() != plan->match_operands) {
    return absl::InvalidArgumentError(
        absl::StrCat("encrypted query has ", operands->size(),
                     " ciphertexts, expected ", plan->match_operands));
  }

  backend::EncryptedQuery query;
  query.op = plan->op;
  query.query = std::move((*operands)[0]);
  for (std::size_t i = 1; i < operands->size(); ++i)
    query.extra_operands.push_back(std::move((*operands)[i]));
  // Always partition matches into result_buckets buckets, independent of the
  // query's LIMIT/OFFSET. The whole partition rides in one ciphertext at no
  // extra cost, so the client decodes every bucket and applies LIMIT/OFFSET as
  // a row skip/take. This keeps LIMIT meaning "rows", not "bucket slots", and
  // lets even a default single-row query return one clean row from a duplicated
  // key instead of a self-collision. A deployment that sets result_buckets = 1
  // opts back into the single-bucket collapse (unique-key only).
  // plan->limit/offset are public and resolved on the client.
  query.result_buckets = result_buckets_;

  // Find the queried column's sub-key inside each packed match record. The
  // planner already verified the column is matchable, so it is searchable.
  const std::optional<std::size_t> rank =
      manifest.schema.SearchableRank(plan->match_column_index);
  if (!rank) {
    return absl::InternalError(
        "planner routed a non-searchable column as the match column");
  }
  const std::size_t match_offset =
      *rank * storage::MatchRecordBytes(manifest.key_bits);

  backend::KeyColumn keys;
  backend::PayloadColumns payload;
  if (absl::Status s = BuildColumns(**reader, match_offset, &keys, &payload);
      !s.ok()) {
    return s;
  }

  absl::StatusOr<std::vector<seal::Ciphertext>> partials =
      (*pir)->evaluate(**eval, query, keys, payload);
  if (!partials.ok())
    return partials.status();

  ShardResult result;
  result.backend_name = plan->backend_name;
  result.partials.reserve(partials->size());
  for (const seal::Ciphertext &cipher : *partials) {
    result.partials.push_back(
        crypto::SerializeCiphertexts(std::vector<seal::Ciphertext>{cipher}));
  }
  return result;
}

absl::StatusOr<std::vector<std::string>> Engine::CombinePartials(
    const std::string &client_id, const std::string &backend_name,
    const std::vector<std::vector<std::string>> &shard_partials) {
  absl::StatusOr<std::unique_ptr<backend::PirBackend>> pir =
      registry_.create(backend_name, ctx_);
  if (!pir.ok())
    return pir.status();
  absl::StatusOr<std::shared_ptr<backend::EvalContext>> eval =
      EvalContextFor(client_id, backend_name, **pir);
  if (!eval.ok())
    return eval.status();

  // Deserialize each shard's partials into ciphertext lists.
  std::vector<std::vector<seal::Ciphertext>> shards;
  shards.reserve(shard_partials.size());
  for (const std::vector<std::string> &shard : shard_partials) {
    std::vector<seal::Ciphertext> ciphers;
    ciphers.reserve(shard.size());
    for (const std::string &blob : shard) {
      // Shard partials are server-produced and have been mod-switched down the
      // modulus chain by the matcher, so they are not fresh top-level operands.
      absl::StatusOr<std::vector<seal::Ciphertext>> one =
          crypto::DeserializeCiphertexts(ctx_, blob, /*max_count=*/1,
                                         /*require_top_level=*/false);
      if (!one.ok())
        return one.status();
      if (one->size() != 1) {
        return absl::InvalidArgumentError("partial is not a single ciphertext");
      }
      ciphers.push_back(std::move((*one)[0]));
    }
    shards.push_back(std::move(ciphers));
  }

  absl::StatusOr<std::vector<seal::Ciphertext>> combined =
      (*pir)->combine(**eval, std::move(shards));
  if (!combined.ok())
    return combined.status();

  // One matched record is its payload planes together, so serialize all planes
  // into a single result blob. An empty result (no rows anywhere) yields none.
  std::vector<std::string> results;
  if (!combined->empty()) {
    results.push_back(crypto::SerializeCiphertexts(*combined));
  }
  return results;
}

absl::StatusOr<Engine::QueryResult>
Engine::Execute(const std::string &client_id, const std::string &database,
                const std::string &sql_template,
                const std::string &encrypted_param,
                const std::string &backend_hint) {
  // Single node: evaluate this node's segment, then combine the one partial
  // set.
  absl::StatusOr<ShardResult> shard =
      EvaluateShard(client_id, database, sql_template, encrypted_param,
                    backend_hint, /*epoch=*/0);
  if (!shard.ok())
    return shard.status();
  absl::StatusOr<std::vector<std::string>> combined =
      CombinePartials(client_id, shard->backend_name, {shard->partials});
  if (!combined.ok())
    return combined.status();
  QueryResult result;
  result.encrypted_results = *std::move(combined);
  return result;
}

} // namespace opaquedb::server

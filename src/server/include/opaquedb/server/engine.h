#ifndef OPAQUEDB_SERVER_ENGINE_H_
#define OPAQUEDB_SERVER_ENGINE_H_

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/admin/keyring_store.h"
#include "opaquedb/backend/pir_backend.h"
#include "opaquedb/backend/registry.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/core/table_id.h"
#include "opaquedb/crypto/context.h"
#include "opaquedb/crypto/key_material.h"
#include "opaquedb/planner/planner.h"
#include "opaquedb/storage/repository.h"

// The Engine facade composes crypto, storage, the planner, and backends for the
// query path on one node. It is transport agnostic: the gRPC service is a thin
// adapter over it. Dependencies are injected so the engine is testable without
// a network or a real disk store.
//
// This is the single-node path. The distributed path (fan out to shard nodes,
// combine under encryption) wraps the same backend evaluate/combine across many
// nodes; the topology here already returns a list and combines partials, so
// that extension does not break this interface.

namespace opaquedb::server {

class Engine {
public:
  // Resolves the repository that holds a given table, so one engine can serve
  // many tables across many databases. Returns a borrowed pointer owned by the
  // caller (the node's repository manager), or a status (NotFound for an
  // unknown table).
  using RepositoryResolver =
      std::function<absl::StatusOr<storage::EpochRepository *>(
          const core::TableId &)>;

  // Single-table engine: every query resolves to the one repository, whatever
  // table it names. Used by tests and the simplest single-table deployments.
  static absl::StatusOr<std::unique_ptr<Engine>>
  Create(const config::CryptoConfig &crypto_cfg, storage::EpochRepository *repo,
         admin::KeyringStore *keyring,
         const backend::BackendRegistry &registry =
             backend::BackendRegistry::instance());

  // Multi-table engine: each query routes to the repository the resolver
  // returns for its (database, table).
  static absl::StatusOr<std::unique_ptr<Engine>>
  Create(const config::CryptoConfig &crypto_cfg, RepositoryResolver resolver,
         admin::KeyringStore *keyring,
         const backend::BackendRegistry &registry =
             backend::BackendRegistry::instance());

  // Validates and stores a client's public and evaluation keys.
  absl::Status RegisterClient(const std::string &client_id,
                              const crypto::KeyMaterial &keys);

  // The schema of a table's current epoch, for clients that decode rows and
  // project. database empty means "default".
  absl::StatusOr<core::Schema> DescribeTable(const std::string &database,
                                             const std::string &table);

  // The outcome of an insert: the new epoch version and the total row count.
  struct InsertOutcome {
    std::uint64_t epoch_version = 0;
    std::uint64_t row_count = 0;
  };

  // Appends one row to a table and publishes a new immutable epoch. cells are
  // the column values as text in the schema's declared column order (every
  // column, including the match key). database empty means "default"; the table
  // must already exist. Plaintext insert today; a future encrypted-payload
  // insert sends pre-encoded values over the same path.
  absl::StatusOr<InsertOutcome>
  InsertRow(const std::string &database, const std::string &table,
            const std::vector<std::string> &cells);

  struct QueryResult {
    // One serialized result ciphertext per matched bucket. One entry today; a
    // list so batch PIR stays non-breaking.
    std::vector<std::string> encrypted_results;
  };

  // Runs one private query against the current epoch on this node alone:
  // evaluate this node's segment then combine. The table comes from the SQL;
  // database selects which database holds it (empty means "default").
  // backend_hint may be empty.
  absl::StatusOr<QueryResult> Execute(const std::string &client_id,
                                      const std::string &database,
                                      const std::string &sql_template,
                                      const std::string &encrypted_param,
                                      const std::string &backend_hint);

  // The result of evaluating one shard: the chosen backend and the serialized
  // partial ciphertexts, before any combine.
  struct ShardResult {
    std::string backend_name;
    std::vector<std::string> partials;
  };

  // Evaluates the predicate over this node's current shard segment and returns
  // serialized partials, without combining. If epoch_version is non-zero it
  // must equal this node's current epoch, so every shard answers one query from
  // the same snapshot. This is what the node-to-node Evaluate RPC calls.
  absl::StatusOr<ShardResult> EvaluateShard(const std::string &client_id,
                                            const std::string &database,
                                            const std::string &sql_template,
                                            const std::string &encrypted_param,
                                            const std::string &backend_hint,
                                            std::uint64_t epoch_version);

  // Combines serialized partials from one or more shards under encryption and
  // returns the serialized result ciphertexts. The coordinating node calls this
  // after gathering partials from every shard.
  absl::StatusOr<std::vector<std::string>>
  CombinePartials(const std::string &client_id, const std::string &backend_name,
                  const std::vector<std::vector<std::string>> &shard_partials);

  const crypto::CryptoContext &crypto_context() const { return ctx_; }

private:
  Engine(crypto::CryptoContext ctx, RepositoryResolver resolver,
         admin::KeyringStore *keyring, const backend::BackendRegistry &registry)
      : ctx_(std::move(ctx)), resolve_repo_(std::move(resolver)),
        keyring_(keyring), registry_(registry), planner_(registry) {}

  // Returns the client's evaluation context for a backend, deserializing the
  // keys once and caching the result. Re-deserializing the (multi-megabyte)
  // keys on every query was a large per-query cost. RegisterClient evicts a
  // client's cached contexts when its keys change.
  absl::StatusOr<std::shared_ptr<backend::EvalContext>>
  EvalContextFor(const std::string &client_id, const std::string &backend_name,
                 backend::PirBackend &backend);

  crypto::CryptoContext ctx_;
  RepositoryResolver resolve_repo_;
  admin::KeyringStore *keyring_;
  const backend::BackendRegistry &registry_;
  planner::Planner planner_;
  std::mutex eval_cache_mu_;
  std::map<std::string, std::shared_ptr<backend::EvalContext>> eval_cache_;
};

} // namespace opaquedb::server

#endif // OPAQUEDB_SERVER_ENGINE_H_

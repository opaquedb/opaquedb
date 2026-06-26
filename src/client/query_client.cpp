#include "opaquedb/client/query_client.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "opaquedb.grpc.pb.h"
#include "opaquedb/core/key_codec.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/core/slot_codec.h"
#include "opaquedb/core/wire.h"
#include "opaquedb/crypto/ops.h"
#include "opaquedb/crypto/serialize.h"
#include "opaquedb/sql/parser.h"

namespace opaquedb::client {
namespace {

grpc::ChannelArguments ChannelArgs(const config::Config &cfg) {
  grpc::ChannelArguments args;
  const int max = static_cast<int>(
      std::min<std::uint64_t>(cfg.server.max_message_bytes, 2147483647ULL));
  args.SetMaxReceiveMessageSize(max);
  args.SetMaxSendMessageSize(max);
  return args;
}

absl::Status FromGrpc(const grpc::Status &s) {
  if (s.ok())
    return absl::OkStatus();
  return absl::Status(static_cast<absl::StatusCode>(s.error_code()),
                      s.error_message());
}

} // namespace

absl::StatusOr<std::unique_ptr<QueryClient>>
QueryClient::Create(const config::Config &cfg, const std::string &target,
                    const std::string &bearer_token) {
  absl::StatusOr<crypto::CryptoContext> c =
      crypto::CryptoContext::Create(cfg.crypto);
  if (!c.ok())
    return c.status();
  // Generate only the Galois steps the matcher uses, not the full set, so the
  // (large) Galois keys stay as small as the algorithm allows.
  crypto::ClientKeyring kr = crypto::ClientKeyring::Generate(
      *c, core::RequiredGaloisSteps(static_cast<std::uint32_t>(c->slot_count()),
                                    cfg.crypto.key_bits));

  auto channel = grpc::CreateCustomChannel(
      target, grpc::InsecureChannelCredentials(), ChannelArgs(cfg));
  auto stub = proto::OpaqueDB::NewStub(channel);

  return std::unique_ptr<QueryClient>(new QueryClient(
      cfg, *std::move(c), std::move(kr), std::move(stub), bearer_token));
}

QueryClient::QueryClient(config::Config cfg, crypto::CryptoContext ctx,
                         crypto::ClientKeyring keyring,
                         std::unique_ptr<opaquedb::proto::OpaqueDB::Stub> stub,
                         std::string bearer)
    : cfg_(std::move(cfg)), ctx_(std::move(ctx)), keyring_(std::move(keyring)),
      stub_(std::move(stub)), bearer_token_(std::move(bearer)) {}

void QueryClient::Authorize(grpc::ClientContext &context) const {
  if (!bearer_token_.empty()) {
    context.AddMetadata("authorization",
                        absl::StrCat("Bearer ", bearer_token_));
  }
}

absl::Status QueryClient::Register(const std::string &client_id) {
  absl::StatusOr<crypto::KeyMaterial> mat = keyring_.SerializePublic();
  if (!mat.ok())
    return mat.status();

  grpc::ClientContext ctx;
  Authorize(ctx);
  opaquedb::proto::RegisterReply reply;
  std::unique_ptr<grpc::ClientWriter<opaquedb::proto::RegisterChunk>> w(
      stub_->Register(&ctx, &reply));

  // Galois keys are large (hundreds of MB at poly 16384), so split each key
  // into chunks well under the gRPC message limit. The server concatenates
  // chunks of the same kind in arrival order. This is what the streaming
  // Register RPC is for; a BlobStore-backed keyring is the documented
  // production path for caching and distributing keys without re-sending them.
  constexpr std::size_t kChunkBytes = 4u * 1024 * 1024;
  auto send = [&](opaquedb::proto::KeyKind k, const std::string &d) -> bool {
    for (std::size_t off = 0; off < d.size(); off += kChunkBytes) {
      opaquedb::proto::RegisterChunk ch;
      ch.set_wire_version(core::kWireVersion);
      ch.set_client_id(client_id);
      ch.set_kind(k);
      ch.set_data(d.substr(off, kChunkBytes));
      if (!w->Write(ch))
        return false;
    }
    return true;
  };

  if (!send(proto::PUBLIC_KEY, mat->public_key) ||
      !send(proto::RELIN_KEYS, mat->relin_keys) ||
      !send(proto::GALOIS_KEYS, mat->galois_keys)) {
    w->WritesDone();
    return FromGrpc(w->Finish());
  }
  w->WritesDone();
  return FromGrpc(w->Finish());
}

absl::StatusOr<core::Schema>
QueryClient::DescribeTable(const std::string &database,
                           const std::string &table) {
  opaquedb::proto::DescribeRequest req;
  req.set_wire_version(core::kWireVersion);
  req.set_database(database);
  req.set_table(table);

  grpc::ClientContext ctx;
  Authorize(ctx);
  opaquedb::proto::DescribeReply reply;
  if (grpc::Status s = stub_->DescribeTable(&ctx, req, &reply); !s.ok()) {
    return FromGrpc(s);
  }
  std::vector<core::Column> columns;
  columns.reserve(static_cast<std::size_t>(reply.columns_size()));
  for (const opaquedb::proto::TableColumn &info : reply.columns()) {
    absl::StatusOr<core::ColumnType> type = core::ParseColumnType(info.type());
    if (!type.ok())
      return type.status();
    absl::StatusOr<core::ColumnEncoding> encoding =
        core::ParseColumnEncoding(info.encoding());
    if (!encoding.ok())
      return encoding.status();
    columns.push_back(core::Column{info.name(), *encoding, *type});
  }
  return core::Schema(reply.table(), std::move(columns));
}

absl::StatusOr<QueryClient::InsertResult>
QueryClient::Insert(const std::string &database, const std::string &table,
                    const std::vector<std::string> &values) {
  opaquedb::proto::InsertRequest req;
  req.set_wire_version(core::kWireVersion);
  req.set_database(database);
  req.set_table(table);
  for (const std::string &v : values)
    req.add_values(v);

  grpc::ClientContext ctx;
  Authorize(ctx);
  opaquedb::proto::InsertReply reply;
  if (grpc::Status s = stub_->Insert(&ctx, req, &reply); !s.ok()) {
    return FromGrpc(s);
  }
  return InsertResult{reply.epoch_version(), reply.row_count()};
}

absl::StatusOr<std::vector<std::vector<std::uint8_t>>>
QueryClient::Query(const std::string &client_id,
                   const std::string &sql_template, std::uint64_t value,
                   const std::string &backend_hint, const std::string &database,
                   std::uint32_t *collided_buckets) {
  // Lift any inline literal out of the template. The literal is the secret; it
  // is encrypted here and the server only ever sees the parameterized form.
  absl::StatusOr<sql::PreparedQuery> prepared =
      sql::PrepareClientQuery(sql_template);
  if (!prepared.ok())
    return prepared.status();

  // The match value is the lifted literal when present, otherwise the bound
  // integer the caller passed.
  const core::Value key = prepared->literal.has_value()
                              ? *prepared->literal
                              : core::Value{static_cast<std::int64_t>(value)};

  absl::StatusOr<core::KeyEncoding> key_value =
      core::EncodeKeyValue(core::ColumnTypeOf(key), key, cfg_.crypto.key_bits);
  if (!key_value.ok())
    return key_value.status();

  absl::StatusOr<std::string> enc = crypto::BuildEncryptedOperand(
      ctx_, keyring_.public_key(), key_value->value, cfg_.crypto.key_bits);
  if (!enc.ok())
    return enc.status();

  opaquedb::proto::QueryRequest req;
  req.set_wire_version(core::kWireVersion);
  req.set_client_id(client_id);
  req.set_sql_template(prepared->sql_template);
  req.set_encrypted_param(*std::move(enc));
  req.set_backend(backend_hint);
  req.set_database(database);

  grpc::ClientContext ctx;
  Authorize(ctx);
  opaquedb::proto::QueryReply reply;
  if (grpc::Status s = stub_->Execute(&ctx, req, &reply); !s.ok()) {
    return FromGrpc(s);
  }

  // The server partitioned matches into result_buckets buckets and packed every
  // bucket into the one result blob. Decode them all into the matched rows (in
  // bucket order, stable across queries), then apply LIMIT/OFFSET as a row
  // skip/take. LIMIT thus counts rows, not bucket slots, and OFFSET pages
  // through the matches. Default is LIMIT 1, OFFSET 0.
  const std::uint32_t buckets = cfg_.crypto.result_buckets;
  const std::uint64_t offset = prepared->offset.value_or(0);
  const std::uint64_t limit = prepared->limit.value_or(1);

  const std::uint32_t bps = cfg_.crypto.BytesPerSlot();
  std::vector<std::vector<std::uint8_t>> matched;
  std::uint32_t collided = 0;
  for (const std::string &blob : reply.encrypted_result()) {
    absl::StatusOr<std::vector<crypto::BucketResult>> recs =
        crypto::DecryptResults(ctx_, keyring_.secret_key(), blob,
                               cfg_.crypto.key_bits, cfg_.storage.record_bytes,
                               bps, buckets, /*offset=*/0, /*limit=*/buckets);
    if (!recs.ok())
      return recs.status();
    for (crypto::BucketResult &br : *recs) {
      if (br.collided) {
        ++collided; // two rows with the same key fell in one bucket
        continue;
      }
      // An absent bucket means no row matched there; only present, clean
      // buckets are matched rows.
      if (br.present)
        matched.push_back(std::move(br.record));
    }
  }
  if (collided_buckets != nullptr)
    *collided_buckets = collided;

  // Apply the public row window: skip `offset` rows, take up to `limit`.
  std::vector<std::vector<std::uint8_t>> out;
  for (std::uint64_t i = offset; i < matched.size() && out.size() < limit;
       ++i) {
    out.push_back(std::move(matched[i]));
  }
  return out;
}

} // namespace opaquedb::client

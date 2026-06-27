#include "opaquedb/client/query_client.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
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
  // When dialing by IP or loopback the certificate host name will not match the
  // target, so let the operator override the name the client verifies against.
  if (!cfg.client.server_name.empty())
    args.SetSslTargetNameOverride(cfg.client.server_name);
  return args;
}

absl::StatusOr<std::string> ReadFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return absl::NotFoundError(absl::StrCat("cannot read '", path, "'"));
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// Channel credentials for the client, chosen by the [client] config. mTLS
// (client cert + key + CA) presents a certificate and verifies the server;
// server-auth TLS (CA only) verifies the server; allow_insecure permits a
// plaintext channel for local development. With none of these set the client
// fails closed rather than sending a bearer token over an unauthenticated
// channel.
absl::StatusOr<std::shared_ptr<grpc::ChannelCredentials>>
MakeClientCredentials(const config::ClientConfig &client) {
  const bool have_client_cert =
      !client.tls_cert.empty() && !client.tls_key.empty();
  if (!client.ca_cert.empty()) {
    grpc::SslCredentialsOptions opts;
    absl::StatusOr<std::string> ca = ReadFile(client.ca_cert);
    if (!ca.ok())
      return ca.status();
    opts.pem_root_certs = *ca;
    if (have_client_cert) {
      absl::StatusOr<std::string> cert = ReadFile(client.tls_cert);
      if (!cert.ok())
        return cert.status();
      absl::StatusOr<std::string> key = ReadFile(client.tls_key);
      if (!key.ok())
        return key.status();
      opts.pem_cert_chain = *cert;
      opts.pem_private_key = *key;
    }
    return grpc::SslCredentials(opts);
  }
  if (client.allow_insecure)
    return grpc::InsecureChannelCredentials();
  return absl::FailedPreconditionError(
      "client transport is not configured: set client.ca_cert to verify the "
      "server over TLS, or client.allow_insecure = true for local development");
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

  absl::StatusOr<std::shared_ptr<grpc::ChannelCredentials>> creds =
      MakeClientCredentials(cfg.client);
  if (!creds.ok())
    return creds.status();
  auto channel = grpc::CreateCustomChannel(target, *creds, ChannelArgs(cfg));
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

absl::StatusOr<QueryClient::Decoded>
QueryClient::RunQuery(const std::string &client_id,
                      const std::string &sql_template, std::uint64_t value,
                      const std::string &backend_hint,
                      const std::string &database) {
  // Lift inline literals out of the template. They are the secret values;
  // encrypted here, the server only ever sees the parameterized form.
  absl::StatusOr<sql::PreparedQuery> prepared =
      sql::PrepareClientQuery(sql_template);
  if (!prepared.ok())
    return prepared.status();

  // The match operands are the lifted literals when present (one for a point
  // query, several for IN / OR), otherwise the single bound value the caller
  // passed.
  std::vector<core::Value> values;
  if (prepared->literals.empty()) {
    values.push_back(core::Value{static_cast<std::int64_t>(value)});
  } else {
    values = prepared->literals;
  }
  std::vector<std::uint64_t> universe;
  universe.reserve(values.size());
  for (const core::Value &v : values) {
    absl::StatusOr<core::KeyEncoding> key_value =
        core::EncodeKeyValue(core::ColumnTypeOf(v), v, cfg_.crypto.key_bits);
    if (!key_value.ok())
      return key_value.status();
    universe.push_back(key_value->value);
  }

  absl::StatusOr<std::string> enc = crypto::BuildEncryptedOperands(
      ctx_, keyring_.public_key(), universe, cfg_.crypto.key_bits);
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
  // bucket into the one result blob. Decode them all (in bucket order, stable
  // across queries); the callers interpret the buckets as rows or as a count.
  const std::uint32_t buckets = cfg_.crypto.result_buckets;
  const std::uint32_t bps = cfg_.crypto.BytesPerSlot();
  Decoded out;
  out.prepared = *std::move(prepared);
  for (const std::string &blob : reply.encrypted_result()) {
    absl::StatusOr<std::vector<crypto::BucketResult>> recs =
        crypto::DecryptResults(ctx_, keyring_.secret_key(), blob,
                               cfg_.crypto.key_bits, cfg_.storage.record_bytes,
                               bps, buckets, /*offset=*/0, /*limit=*/buckets);
    if (!recs.ok())
      return recs.status();
    for (crypto::BucketResult &br : *recs)
      out.buckets.push_back(std::move(br));
  }
  return out;
}

absl::StatusOr<std::vector<std::vector<std::uint8_t>>>
QueryClient::Query(const std::string &client_id,
                   const std::string &sql_template, std::uint64_t value,
                   const std::string &backend_hint, const std::string &database,
                   std::uint32_t *collided_buckets) {
  absl::StatusOr<Decoded> decoded =
      RunQuery(client_id, sql_template, value, backend_hint, database);
  if (!decoded.ok())
    return decoded.status();

  std::vector<std::vector<std::uint8_t>> matched;
  std::uint32_t collided = 0;
  for (crypto::BucketResult &br : decoded->buckets) {
    if (br.collided) {
      ++collided; // two rows with the same key fell in one bucket
      continue;
    }
    // An absent bucket means no row matched there; only present, clean buckets
    // are matched rows.
    if (br.present)
      matched.push_back(std::move(br.record));
  }
  if (collided_buckets != nullptr)
    *collided_buckets = collided;

  // Apply the public row window: skip `offset` rows, take up to `limit`. LIMIT
  // counts rows, not bucket slots; default is LIMIT 1, OFFSET 0.
  const std::uint64_t offset = decoded->prepared.offset.value_or(0);
  const std::uint64_t limit = decoded->prepared.limit.value_or(1);
  std::vector<std::vector<std::uint8_t>> out;
  for (std::uint64_t i = offset; i < matched.size() && out.size() < limit;
       ++i) {
    out.push_back(std::move(matched[i]));
  }
  return out;
}

absl::StatusOr<std::uint64_t>
QueryClient::QueryCount(const std::string &client_id,
                        const std::string &sql_template, std::uint64_t value,
                        const std::string &backend_hint,
                        const std::string &database) {
  absl::StatusOr<Decoded> decoded =
      RunQuery(client_id, sql_template, value, backend_hint, database);
  if (!decoded.ok())
    return decoded.status();
  // Every matching row contributes 1 to exactly one bucket's presence count, so
  // summing the per-bucket counts is the exact total even when rows collide in
  // a bucket (a collision corrupts the payload bytes, not the count).
  std::uint64_t total = 0;
  for (const crypto::BucketResult &br : decoded->buckets)
    total += br.count;
  return total;
}

} // namespace opaquedb::client

#include "opaquedb/server/query_service.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"
#include "internal.grpc.pb.h"
#include "opaquedb/core/wire.h"
#include "opaquedb/server/query_coordinator.h"
#include "spdlog/spdlog.h"

namespace opaquedb::server {
namespace {

// A channel to a peer shard (Internal RPC), sized for query and key uploads.
// Credentials are provided by the node (insecure for tests/none, or Ssl when
// TLS/mTLS configured on the server).
std::shared_ptr<grpc::Channel>
PeerChannel(const std::string &address, std::uint64_t max_message_bytes,
            std::shared_ptr<grpc::ChannelCredentials> creds) {
  grpc::ChannelArguments args;
  const int max_bytes = static_cast<int>(
      std::min<std::uint64_t>(max_message_bytes, 2147483647ULL));
  args.SetMaxReceiveMessageSize(max_bytes);
  args.SetMaxSendMessageSize(max_bytes);
  if (!creds)
    creds = grpc::InsecureChannelCredentials();
  return grpc::CreateCustomChannel(address, std::move(creds), args);
}

} // namespace

grpc::Status QueryService::CheckClientOwnership(const std::string &client_id,
                                                const std::string &principal_id,
                                                bool claim) {
  std::lock_guard<std::mutex> lock(owners_mu_);
  auto it = client_owner_.find(client_id);
  if (it != client_owner_.end() && it->second != principal_id) {
    return ToGrpcStatus(absl::PermissionDeniedError(
        "client id is registered to another principal"));
  }
  if (claim && it == client_owner_.end())
    client_owner_.emplace(client_id, principal_id);
  return grpc::Status::OK;
}

std::shared_ptr<grpc::Channel>
QueryService::PeerChannelFor(const std::string &address) {
  std::lock_guard<std::mutex> lock(channels_mu_);
  auto it = channels_.find(address);
  if (it != channels_.end())
    return it->second;
  std::shared_ptr<grpc::Channel> channel =
      PeerChannel(address, max_message_bytes_, peer_creds_);
  channels_.emplace(address, channel);
  return channel;
}

grpc::Status ToGrpcStatus(const absl::Status &status) {
  if (status.ok())
    return grpc::Status::OK;
  // absl and gRPC share the canonical code numbering.
  return grpc::Status(static_cast<grpc::StatusCode>(status.code()),
                      std::string(status.message()));
}

auth::AuthInputs ExtractAuthInputs(const grpc::ServerContext &context) {
  auth::AuthInputs inputs;
  const auto &metadata = context.client_metadata();
  auto it = metadata.find("authorization");
  if (it != metadata.end()) {
    const std::string value(it->second.data(), it->second.size());
    constexpr std::string_view kBearer = "Bearer ";
    if (value.size() > kBearer.size() &&
        value.compare(0, kBearer.size(), kBearer) == 0) {
      inputs.bearer_token = value.substr(kBearer.size());
    }
  }
  const std::shared_ptr<const grpc::AuthContext> auth = context.auth_context();
  if (auth != nullptr) {
    const std::vector<grpc::string_ref> ids = auth->GetPeerIdentity();
    if (!ids.empty()) {
      inputs.peer_identity = std::string(ids[0].data(), ids[0].size());
    }
  }
  return inputs;
}

grpc::Status
QueryService::Register(grpc::ServerContext *context,
                       grpc::ServerReader<proto::RegisterChunk> *reader,
                       proto::RegisterReply *reply) {
  absl::StatusOr<auth::Principal> principal =
      gate_->Check(ExtractAuthInputs(*context), auth::Role::kQuery);
  if (!principal.ok()) {
    return ToGrpcStatus(principal.status());
  }
  std::string client_id;
  std::string public_key;
  std::string relin_keys;
  std::string galois_keys;
  bool seen = false;

  proto::RegisterChunk chunk;
  while (reader->Read(&chunk)) {
    if (chunk.wire_version() != core::kWireVersion) {
      return ToGrpcStatus(absl::FailedPreconditionError(absl::StrCat(
          "wire version ", chunk.wire_version(), " not supported")));
    }
    if (chunk.client_id().empty()) {
      return ToGrpcStatus(
          absl::InvalidArgumentError("register chunk has empty client id"));
    }
    if (!seen) {
      client_id = chunk.client_id();
      seen = true;
    } else if (chunk.client_id() != client_id) {
      return ToGrpcStatus(absl::InvalidArgumentError(
          "register chunks disagree on the client id"));
    }
    switch (chunk.kind()) {
    case proto::PUBLIC_KEY:
      public_key.append(chunk.data());
      break;
    case proto::RELIN_KEYS:
      relin_keys.append(chunk.data());
      break;
    case proto::GALOIS_KEYS:
      galois_keys.append(chunk.data());
      break;
    default:
      return ToGrpcStatus(absl::InvalidArgumentError(
          "register chunk has an unspecified key kind"));
    }
  }
  if (!seen) {
    return ToGrpcStatus(
        absl::InvalidArgumentError("register stream carried no chunks"));
  }
  // Bind the client id to this principal, or reject if another principal owns
  // it, so a caller cannot overwrite someone else's keyring entry.
  if (grpc::Status s =
          CheckClientOwnership(client_id, principal->id, /*claim=*/true);
      !s.ok()) {
    return s;
  }

  crypto::KeyMaterial keys;
  keys.public_key = std::move(public_key);
  keys.relin_keys = std::move(relin_keys);
  keys.galois_keys = std::move(galois_keys);
  if (absl::Status s = engine_->RegisterClient(client_id, keys); !s.ok()) {
    return ToGrpcStatus(s);
  }
  spdlog::info("audit: register principal={} client={}", principal->id,
               client_id);

  // The node that received this Register is the coordinator for the client: it
  // stored the keys locally above and now distributes them to every shard peer
  // so each can evaluate this client's query. Key distribution is always the
  // coordinator's responsibility; shards never fetch keys themselves.
  //
  // Keys are chunked over the streaming RegisterKeys RPC because the Galois set
  // is large (~110 MB at poly 16384, even reduced to the steps the matcher
  // uses). TODO(blobstore): the production path is a shared object store
  // (MinIO / S3, see BlobStoreConfig) that the coordinator writes the keys to
  // once, keyed by client id; shards read them by reference instead of having
  // the ~110 MB re-streamed to every shard on every registration.
  // A shard that already has the keys just overwrites them.
  constexpr std::size_t kChunkBytes = 4u * 1024 * 1024;
  auto forward_key = [&](grpc::ClientWriter<proto::KeyUploadChunk> *w,
                         proto::KeyKind kind, const std::string &data) -> bool {
    for (std::size_t off = 0; off < data.size(); off += kChunkBytes) {
      proto::KeyUploadChunk ch;
      ch.set_wire_version(core::kWireVersion);
      ch.set_client_id(client_id);
      ch.set_kind(kind);
      ch.set_data(data.substr(off, kChunkBytes));
      if (!w->Write(ch))
        return false;
    }
    return true;
  };
  const std::vector<std::string> key_peers = peers_();
  if (!key_peers.empty()) {
    spdlog::info("distributing keys for client {} to {} peer(s)", client_id,
                 key_peers.size());
  }
  for (const std::string &peer : key_peers) {
    auto stub = proto::Internal::NewStub(PeerChannelFor(peer));
    grpc::ClientContext ctx;
    proto::KeyUploadReply ack;
    std::unique_ptr<grpc::ClientWriter<proto::KeyUploadChunk>> w(
        stub->RegisterKeys(&ctx, &ack));
    if (!forward_key(w.get(), proto::PUBLIC_KEY, keys.public_key) ||
        !forward_key(w.get(), proto::RELIN_KEYS, keys.relin_keys) ||
        !forward_key(w.get(), proto::GALOIS_KEYS, keys.galois_keys)) {
      w->WritesDone();
      (void)w->Finish();
      return ToGrpcStatus(absl::UnavailableError(
          absl::StrCat("failed to distribute keys to ", peer)));
    }
    w->WritesDone();
    if (grpc::Status s = w->Finish(); !s.ok()) {
      return ToGrpcStatus(absl::UnavailableError(absl::StrCat(
          "failed to distribute keys to ", peer, ": ", s.error_message())));
    }
  }
  reply->set_client_id(client_id);
  return grpc::Status::OK;
}

grpc::Status QueryService::Execute(grpc::ServerContext *context,
                                   const proto::QueryRequest *request,
                                   proto::QueryReply *reply) {
  absl::StatusOr<auth::Principal> principal =
      gate_->Check(ExtractAuthInputs(*context), auth::Role::kQuery);
  if (!principal.ok()) {
    return ToGrpcStatus(principal.status());
  }
  if (request->wire_version() != core::kWireVersion) {
    return ToGrpcStatus(absl::FailedPreconditionError(absl::StrCat(
        "wire version ", request->wire_version(), " not supported")));
  }
  // Only the principal that registered this client id may query under it.
  if (grpc::Status s = CheckClientOwnership(request->client_id(), principal->id,
                                            /*claim=*/false);
      !s.ok()) {
    return s;
  }

  // With shard peers, coordinate: evaluate the local shard, fan the same query
  // out to the peers at one pinned epoch, and combine. With no peers, answer
  // from this node alone.
  const std::vector<std::string> peers = peers_();
  absl::StatusOr<Engine::QueryResult> result = absl::UnknownError("unset");
  if (peers.empty()) {
    spdlog::info(
        "audit: query principal={} client={} database={} (single-node)",
        principal->id, request->client_id(), request->database());
    result = engine_->Execute(request->client_id(), request->database(),
                              request->sql_template(),
                              request->encrypted_param(), request->backend());
  } else {
    spdlog::info("audit: query principal={} client={} database={} "
                 "(coordinating {} peers)",
                 principal->id, request->client_id(), request->database(),
                 peers.size());
    std::vector<std::shared_ptr<grpc::Channel>> channels;
    channels.reserve(peers.size());
    for (const std::string &peer : peers) {
      channels.push_back(PeerChannelFor(peer));
    }
    QueryCoordinator coordinator(engine_, std::move(channels), epoch_(),
                                 max_message_bytes_);
    result = coordinator.Execute(
        request->client_id(), request->database(), request->sql_template(),
        request->encrypted_param(), request->backend());
  }
  if (!result.ok()) {
    spdlog::warn("query from client {} failed: {}", request->client_id(),
                 result.status().message());
    return ToGrpcStatus(result.status());
  }
  // The count is encrypted result blobs, not rows: all result buckets pack into
  // one blob and the operator never learns how many rows the client decodes.
  spdlog::info("query from client {} returned {} encrypted result(s)",
               request->client_id(), result->encrypted_results.size());
  for (std::string &bytes : result->encrypted_results) {
    reply->add_encrypted_result(std::move(bytes));
  }
  return grpc::Status::OK;
}

grpc::Status QueryService::Insert(grpc::ServerContext *context,
                                  const proto::InsertRequest *request,
                                  proto::InsertReply *reply) {
  // Insert mutates the table, so it requires the Admin role; a read-only Query
  // principal cannot write.
  absl::StatusOr<auth::Principal> principal =
      gate_->Check(ExtractAuthInputs(*context), auth::Role::kAdmin);
  if (!principal.ok()) {
    return ToGrpcStatus(principal.status());
  }
  if (request->wire_version() != core::kWireVersion) {
    return ToGrpcStatus(absl::FailedPreconditionError(absl::StrCat(
        "wire version ", request->wire_version(), " not supported")));
  }
  const std::string db =
      request->database().empty() ? "default" : request->database();
  std::vector<std::string> cells(request->values().begin(),
                                 request->values().end());
  absl::StatusOr<Engine::InsertOutcome> outcome =
      engine_->InsertRow(request->database(), request->table(), cells);
  if (!outcome.ok()) {
    spdlog::warn("insert into {}.{} failed: {}", db, request->table(),
                 outcome.status().message());
    return ToGrpcStatus(outcome.status());
  }
  spdlog::info(
      "audit: insert principal={} into {}.{} ({} rows total, epoch {})",
      principal->id, db, request->table(), outcome->row_count,
      outcome->epoch_version);
  reply->set_epoch_version(outcome->epoch_version);
  reply->set_row_count(outcome->row_count);
  return grpc::Status::OK;
}

grpc::Status QueryService::DescribeTable(grpc::ServerContext *context,
                                         const proto::DescribeRequest *request,
                                         proto::DescribeReply *reply) {
  if (absl::StatusOr<auth::Principal> principal =
          gate_->Check(ExtractAuthInputs(*context), auth::Role::kQuery);
      !principal.ok()) {
    return ToGrpcStatus(principal.status());
  }
  if (request->wire_version() != core::kWireVersion) {
    return ToGrpcStatus(absl::FailedPreconditionError(absl::StrCat(
        "wire version ", request->wire_version(), " not supported")));
  }
  absl::StatusOr<core::Schema> schema =
      engine_->DescribeTable(request->database(), request->table());
  if (!schema.ok())
    return ToGrpcStatus(schema.status());
  reply->set_table(schema->table());
  for (const core::Column &col : schema->columns()) {
    proto::TableColumn *info = reply->add_columns();
    info->set_name(col.name);
    info->set_type(core::ToString(col.type));
    info->set_encoding(core::ToString(col.encoding));
  }
  return grpc::Status::OK;
}

} // namespace opaquedb::server

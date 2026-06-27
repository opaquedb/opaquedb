#include "opaquedb/server/shard_service.h"

#include "absl/strings/str_cat.h"
#include "opaquedb/core/wire.h"
#include "opaquedb/crypto/key_material.h"
#include "opaquedb/server/query_service.h" // ToGrpcStatus, ExtractAuthInputs
#include "spdlog/spdlog.h"

namespace opaquedb::server {

grpc::Status
ShardService::AuthorizePeer(const grpc::ServerContext &context) const {
  if (!require_peer_cert_)
    return grpc::Status::OK;
  const auth::AuthInputs inputs = ExtractAuthInputs(context);
  if (!inputs.peer_identity || inputs.peer_identity->empty()) {
    spdlog::warn("audit: internal RPC rejected: no verified peer certificate");
    return ToGrpcStatus(absl::PermissionDeniedError(
        "node-to-node RPC requires a verified cluster certificate"));
  }
  return grpc::Status::OK;
}

grpc::Status ShardService::Evaluate(grpc::ServerContext *context,
                                    const proto::ShardQuery *request,
                                    proto::ShardPartial *reply) {
  if (grpc::Status s = AuthorizePeer(*context); !s.ok())
    return s;
  if (request->wire_version() != core::kWireVersion) {
    return ToGrpcStatus(absl::FailedPreconditionError(absl::StrCat(
        "wire version ", request->wire_version(), " not supported")));
  }
  absl::StatusOr<Engine::ShardResult> result = engine_->EvaluateShard(
      request->client_id(), request->database(), request->sql_template(),
      request->encrypted_param(), request->backend(), request->epoch_version());
  if (!result.ok())
    return ToGrpcStatus(result.status());
  for (std::string &partial : result->partials) {
    reply->add_partials(std::move(partial));
  }
  return grpc::Status::OK;
}

grpc::Status
ShardService::RegisterKeys(grpc::ServerContext *context,
                           grpc::ServerReader<proto::KeyUploadChunk> *reader,
                           proto::KeyUploadReply *reply) {
  if (grpc::Status s = AuthorizePeer(*context); !s.ok())
    return s;
  std::string client_id;
  crypto::KeyMaterial keys;
  bool seen = false;

  proto::KeyUploadChunk chunk;
  while (reader->Read(&chunk)) {
    if (chunk.wire_version() != core::kWireVersion) {
      return ToGrpcStatus(absl::FailedPreconditionError(absl::StrCat(
          "wire version ", chunk.wire_version(), " not supported")));
    }
    if (chunk.client_id().empty()) {
      return ToGrpcStatus(
          absl::InvalidArgumentError("key upload chunk has empty client id"));
    }
    if (!seen) {
      client_id = chunk.client_id();
      seen = true;
    } else if (chunk.client_id() != client_id) {
      return ToGrpcStatus(absl::InvalidArgumentError(
          "key upload chunks disagree on the client id"));
    }
    switch (chunk.kind()) {
    case proto::PUBLIC_KEY:
      keys.public_key.append(chunk.data());
      break;
    case proto::RELIN_KEYS:
      keys.relin_keys.append(chunk.data());
      break;
    case proto::GALOIS_KEYS:
      keys.galois_keys.append(chunk.data());
      break;
    default:
      return ToGrpcStatus(absl::InvalidArgumentError(
          "key upload chunk has an unspecified key kind"));
    }
  }
  if (!seen) {
    return ToGrpcStatus(
        absl::InvalidArgumentError("key upload stream carried no chunks"));
  }
  if (absl::Status s = engine_->RegisterClient(client_id, keys); !s.ok()) {
    return ToGrpcStatus(s);
  }
  spdlog::info("shard received keys for client {} from coordinator", client_id);
  reply->set_client_id(client_id);
  return grpc::Status::OK;
}

} // namespace opaquedb::server

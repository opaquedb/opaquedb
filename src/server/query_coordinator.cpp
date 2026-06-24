#include "opaquedb/server/query_coordinator.h"

#include <utility>

#include "absl/strings/str_cat.h"
#include "internal.grpc.pb.h"
#include "opaquedb/core/wire.h"

namespace opaquedb::server {
namespace {

absl::Status FromGrpc(const grpc::Status &status) {
  if (status.ok())
    return absl::OkStatus();
  return absl::Status(static_cast<absl::StatusCode>(status.error_code()),
                      status.error_message());
}

} // namespace

QueryCoordinator::QueryCoordinator(
    Engine *engine, std::vector<std::shared_ptr<grpc::Channel>> peer_channels,
    std::uint64_t epoch_version, std::uint64_t /*max_message_bytes*/)
    : engine_(engine), peer_channels_(std::move(peer_channels)),
      epoch_version_(epoch_version) {}

absl::StatusOr<Engine::QueryResult> QueryCoordinator::Execute(
    const std::string &client_id, const std::string &database,
    const std::string &sql_template, const std::string &encrypted_param,
    const std::string &backend_hint) {
  // Evaluate this node's own shard first; it also tells us the chosen backend.
  absl::StatusOr<Engine::ShardResult> local =
      engine_->EvaluateShard(client_id, database, sql_template, encrypted_param,
                             backend_hint, epoch_version_);
  if (!local.ok())
    return local.status();

  std::vector<std::vector<std::string>> shard_partials;
  shard_partials.push_back(local->partials);

  // Fan the same query out to every peer shard at the pinned epoch.
  for (const std::shared_ptr<grpc::Channel> &channel : peer_channels_) {
    std::unique_ptr<proto::Internal::Stub> stub =
        proto::Internal::NewStub(channel);
    proto::ShardQuery query;
    query.set_wire_version(core::kWireVersion);
    query.set_client_id(client_id);
    query.set_sql_template(sql_template);
    query.set_encrypted_param(encrypted_param);
    query.set_backend(backend_hint);
    query.set_epoch_version(epoch_version_);
    query.set_database(database);

    grpc::ClientContext context;
    proto::ShardPartial reply;
    if (grpc::Status s = stub->Evaluate(&context, query, &reply); !s.ok()) {
      return FromGrpc(s);
    }
    std::vector<std::string> partials;
    partials.reserve(static_cast<std::size_t>(reply.partials_size()));
    for (const std::string &p : reply.partials())
      partials.push_back(p);
    shard_partials.push_back(std::move(partials));
  }

  absl::StatusOr<std::vector<std::string>> combined =
      engine_->CombinePartials(client_id, local->backend_name, shard_partials);
  if (!combined.ok())
    return combined.status();
  Engine::QueryResult result;
  result.encrypted_results = *std::move(combined);
  return result;
}

} // namespace opaquedb::server

#include "opaquedb/server/query_coordinator.h"

#include <thread>
#include <utility>
#include <vector>

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
  // Every shard does the same linear scan, so the slow part is the per-shard
  // FHE evaluation, not the coordination. Run them all at once: this node's own
  // shard on one thread while each peer's Evaluate RPC runs on its own. Wall
  // clock is the slowest single shard, not the sum. Each worker writes only its
  // own result slot, so no locking is needed.
  const std::size_t npeers = peer_channels_.size();

  absl::StatusOr<Engine::ShardResult> local = absl::UnknownError("unset");
  std::thread local_thread([&]() {
    local = engine_->EvaluateShard(client_id, database, sql_template,
                                   encrypted_param, backend_hint, epoch_version_);
  });

  std::vector<std::vector<std::string>> peer_partials(npeers);
  std::vector<absl::Status> peer_status(npeers, absl::OkStatus());
  std::vector<std::thread> peer_threads;
  peer_threads.reserve(npeers);
  for (std::size_t i = 0; i < npeers; ++i) {
    peer_threads.emplace_back([&, i]() {
      std::unique_ptr<proto::Internal::Stub> stub =
          proto::Internal::NewStub(peer_channels_[i]);
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
        peer_status[i] = FromGrpc(s);
        return;
      }
      peer_partials[i].reserve(static_cast<std::size_t>(reply.partials_size()));
      for (const std::string &p : reply.partials())
        peer_partials[i].push_back(p);
    });
  }

  local_thread.join();
  for (std::thread &t : peer_threads)
    t.join();

  // Surface the first failure. A missing shard would silently undercount the
  // combined sum, so a shard error fails the whole query rather than returning
  // a wrong answer.
  if (!local.ok())
    return local.status();
  for (const absl::Status &s : peer_status)
    if (!s.ok())
      return s;

  std::vector<std::vector<std::string>> shard_partials;
  shard_partials.reserve(npeers + 1);
  shard_partials.push_back(std::move(local->partials));
  for (std::vector<std::string> &p : peer_partials)
    shard_partials.push_back(std::move(p));

  absl::StatusOr<std::vector<std::string>> combined =
      engine_->CombinePartials(client_id, local->backend_name, shard_partials);
  if (!combined.ok())
    return combined.status();
  Engine::QueryResult result;
  result.encrypted_results = *std::move(combined);
  return result;
}

} // namespace opaquedb::server

#ifndef OPAQUEDB_SERVER_QUERY_COORDINATOR_H_
#define OPAQUEDB_SERVER_QUERY_COORDINATOR_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "grpcpp/grpcpp.h"
#include "opaquedb/server/engine.h"

// Coordinates a distributed query. Any node can coordinate: it evaluates its
// own shard locally, fans the same encrypted query out to the peer shard nodes
// over the internal Evaluate RPC at one pinned epoch, gathers the encrypted
// partials, and combines them under encryption. There is no single coordinator
// and no query bottleneck; sharding spreads the linear scan but does not reduce
// the total work, and the result decrypts only on the client.

namespace opaquedb::server {

class QueryCoordinator {
public:
  // peer_channels are channels to the other shard nodes (not this one).
  // epoch_version, if non-zero, is the snapshot every shard must answer from.
  QueryCoordinator(Engine *engine,
                   std::vector<std::shared_ptr<grpc::Channel>> peer_channels,
                   std::uint64_t epoch_version,
                   std::uint64_t max_message_bytes);

  absl::StatusOr<Engine::QueryResult>
  Execute(const std::string &client_id, const std::string &database,
          const std::string &sql_template, const std::string &encrypted_param,
          const std::string &backend_hint);

private:
  Engine *engine_;
  std::vector<std::shared_ptr<grpc::Channel>> peer_channels_;
  std::uint64_t epoch_version_;
};

} // namespace opaquedb::server

#endif // OPAQUEDB_SERVER_QUERY_COORDINATOR_H_

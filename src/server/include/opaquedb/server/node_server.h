#ifndef OPAQUEDB_SERVER_NODE_SERVER_H_
#define OPAQUEDB_SERVER_NODE_SERVER_H_

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "grpcpp/grpcpp.h"
#include "opaquedb/admin/keyring_store.h"
#include "opaquedb/admin/service.h"
#include "opaquedb/auth/authenticator.h"
#include "opaquedb/cluster/etcd_client.h"
#include "opaquedb/cluster/manager.h"
#include "opaquedb/config/config.h"
#include "opaquedb/server/admin_grpc_service.h"
#include "opaquedb/server/engine.h"
#include "opaquedb/server/query_service.h"
#include "opaquedb/server/repository_manager.h"
#include "opaquedb/server/request_gate.h"
#include "opaquedb/server/shard_service.h"
#include "opaquedb/storage/repository.h"

// A single node: it owns the epoch repository, the keyring, the engine, and the
// gRPC server that exposes the client query API. The cluster wiring (etcd,
// shard map, internal RPC) is layered on later; this is the query-serving core
// that every node runs.

namespace opaquedb::server {

class NodeServer {
public:
  static absl::StatusOr<std::unique_ptr<NodeServer>>
  Create(config::Config config);

  // Builds the engine and starts the gRPC server listening on the configured
  // address. Returns once the server is accepting connections.
  absl::Status Start();

  // Blocks until Shutdown is called from another thread.
  void Wait();

  // Stops accepting requests and drains in-flight ones.
  void Shutdown();

  // Sets the shard peer addresses this node fans queries out to. Used for
  // static deployments and tests; when clustering is enabled, peers come from
  // etcd membership instead.
  void SetQueryPeers(std::vector<std::string> peers);

  // Rebuilds the authenticator from config (re-reading the token file) and
  // swaps it into the request gate without dropping connections. Called on
  // SIGHUP so a token rotation takes effect without a restart. Returns an error
  // if the new config cannot build an authenticator; the old one stays in use.
  absl::Status ReloadAuth();

  const std::string &listen_address() const { return listen_address_; }
  // The node-to-node listener address. Equals listen_address() when the
  // Internal service shares the client listener (cluster.listen unset).
  const std::string &cluster_listen_address() const {
    return cluster_listen_address_;
  }
  Engine *engine() { return engine_.get(); }

  // Best-effort wait until the server is listening and accepting (useful in
  // tests to avoid races with gRPC startup on ephemeral ports).
  absl::Status
  WaitForReady(std::chrono::milliseconds timeout = std::chrono::seconds(5));

private:
  explicit NodeServer(config::Config config) : config_(std::move(config)) {}

  // The peer addresses (excluding self) and pinned epoch read from etcd, cached
  // for a short TTL. The query resolvers run per request; without this every
  // query did an etcd membership scan and an epoch read, coupling query latency
  // and availability to etcd. Only used when clustering is enabled.
  struct ClusterView {
    std::vector<std::string> peers;
    std::uint64_t epoch = 0;
  };
  ClusterView ClusterViewCached();

  config::Config config_;
  std::string listen_address_;
  std::string advertise_address_;
  // The node-to-node listener and the address peers dial for it. When
  // cluster.listen is set these name the separate Internal listener; otherwise
  // they mirror the client listener/advertise (shared listener).
  std::string cluster_listen_address_;
  std::string cluster_advertise_address_;
  mutable std::mutex peers_mutex_;
  std::vector<std::string> query_peers_;
  std::mutex cluster_cache_mu_;
  ClusterView cluster_cache_;
  std::chrono::steady_clock::time_point cluster_cache_at_{};
  bool cluster_cache_valid_ = false;
  std::unique_ptr<storage::EpochRepository> repo_;
  std::unique_ptr<RepositoryManager> repo_manager_;
  std::unique_ptr<admin::KeyringStore> keyring_;
  std::shared_ptr<auth::Authenticator> authenticator_;
  std::unique_ptr<RequestGate> gate_;
  std::unique_ptr<Engine> engine_;
  std::unique_ptr<QueryService> service_;
  std::unique_ptr<admin::AdminService> admin_;
  std::unique_ptr<AdminGrpcService> admin_service_;
  std::unique_ptr<ShardService> shard_service_;
  std::unique_ptr<cluster::EtcdClient> etcd_;
  std::unique_ptr<cluster::ClusterManager> cluster_;
  std::unique_ptr<grpc::Server> server_;
  // The separate node-to-node server, present only when cluster.listen is set.
  std::unique_ptr<grpc::Server> cluster_server_;
  std::shared_ptr<grpc::ChannelCredentials> peer_creds_;
};

} // namespace opaquedb::server

#endif // OPAQUEDB_SERVER_NODE_SERVER_H_

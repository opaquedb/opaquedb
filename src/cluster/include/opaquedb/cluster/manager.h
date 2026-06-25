#ifndef OPAQUEDB_CLUSTER_MANAGER_H_
#define OPAQUEDB_CLUSTER_MANAGER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/cluster/etcd_client.h"
#include "opaquedb/cluster/shard_map.h"

// The cluster manager self-organizes a node through the EtcdClient. Every node
// holds a lease, registers itself under a membership key, and contends for the
// single leader key by an atomic create. The leader owns only control-plane
// duties: it recomputes and publishes the consistent-hash shard map when
// membership changes, and it advances the global epoch version. The leader is
// not on the query hot path; any node can coordinate a query.
//
// One maintenance step is Tick(); production runs it on a background thread,
// and tests call it directly with the fake etcd's virtual clock so leader
// handoff is deterministic.

namespace opaquedb::cluster {

struct NodeInfo {
  std::string id;
  std::string address;
};

struct ClusterOptions {
  std::string node_id;
  std::string node_address; // address peers reach this node on
  std::string leader_key = "/opaquedb/leader";
  int lease_ttl_seconds = 10;
  int virtual_nodes = 128;
};

class ClusterManager {
public:
  ClusterManager(EtcdClient *etcd, ClusterOptions options);
  ~ClusterManager();

  // Runs one maintenance step: refresh the lease, register membership, contend
  // for leadership, and (as leader) republish the shard map if it changed.
  absl::Status Tick();

  bool is_leader() const { return is_leader_.load(); }
  absl::StatusOr<std::optional<std::string>> CurrentLeader();
  absl::StatusOr<std::vector<NodeInfo>> Members();
  absl::StatusOr<ShardMap> CurrentShardMap();

  // Epoch coordination. Any node reads the pinned global epoch; only the leader
  // advances it.
  absl::StatusOr<std::uint64_t> CurrentEpoch();
  absl::Status AdvanceEpoch(std::uint64_t version);

  // Starts/stops a background thread that ticks every ttl/3 seconds. Stop also
  // revokes the lease so the node leaves the cluster promptly.
  void Start();
  void Stop();

private:
  std::string MembersPrefix() const;
  std::string ShardMapKey() const;
  std::string EpochKey() const;
  absl::Status EnsureLease();

  EtcdClient *etcd_;
  ClusterOptions options_;
  std::string base_;

  std::mutex mutex_;
  LeaseId lease_ = 0;
  std::string published_shard_map_;
  std::atomic<bool> is_leader_{false};

  std::atomic<bool> running_{false};
  std::thread worker_;
};

} // namespace opaquedb::cluster

#endif // OPAQUEDB_CLUSTER_MANAGER_H_

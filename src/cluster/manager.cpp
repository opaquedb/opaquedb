#include "opaquedb/cluster/manager.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "spdlog/spdlog.h"

namespace opaquedb::cluster {
namespace {

// The base prefix is the leader key without its final segment, e.g.
// "/opaquedb/leader" -> "/opaquedb". Membership, shard map, and epoch keys hang
// off it so one config value places the whole cluster namespace.
std::string BaseOf(const std::string &leader_key) {
  const std::size_t pos = leader_key.rfind('/');
  return pos == std::string::npos ? std::string() : leader_key.substr(0, pos);
}

} // namespace

ClusterManager::ClusterManager(EtcdClient *etcd, ClusterOptions options)
    : etcd_(etcd), options_(std::move(options)),
      base_(BaseOf(options_.leader_key)) {}

ClusterManager::~ClusterManager() { Stop(); }

std::string ClusterManager::MembersPrefix() const {
  return absl::StrCat(base_, "/members/");
}
std::string ClusterManager::ShardMapKey() const {
  return absl::StrCat(base_, "/shardmap");
}
std::string ClusterManager::EpochKey() const {
  return absl::StrCat(base_, "/epoch");
}

absl::Status ClusterManager::EnsureLease() {
  if (lease_ != 0) {
    if (absl::Status s = etcd_->LeaseKeepAlive(lease_); s.ok()) {
      return absl::OkStatus();
    }
    // The lease expired; drop leadership and regrant below.
    spdlog::warn("cluster: lease lapsed, regranting and dropping leadership");
    lease_ = 0;
    is_leader_.store(false);
  }
  absl::StatusOr<LeaseId> lease = etcd_->LeaseGrant(options_.lease_ttl_seconds);
  if (!lease.ok())
    return lease.status();
  lease_ = *lease;
  spdlog::info("cluster: lease granted (id {})", lease_);
  return absl::OkStatus();
}

absl::Status ClusterManager::Tick() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (absl::Status s = EnsureLease(); !s.ok())
    return s;

  // Register or refresh this node's membership entry under our lease.
  if (absl::Status s =
          etcd_->Put(absl::StrCat(MembersPrefix(), options_.node_id),
                     options_.node_address, lease_);
      !s.ok()) {
    return s;
  }

  // Contend for leadership. If the key is free, an atomic create decides the
  // winner; otherwise we are leader iff the key already holds our id.
  absl::StatusOr<std::optional<std::string>> leader =
      etcd_->Get(options_.leader_key);
  if (!leader.ok())
    return leader.status();
  const bool was_leader = is_leader_.load();
  if (!leader->has_value()) {
    absl::StatusOr<bool> won =
        etcd_->CreateIfAbsent(options_.leader_key, options_.node_id, lease_);
    if (!won.ok())
      return won.status();
    is_leader_.store(*won);
  } else {
    is_leader_.store(**leader == options_.node_id);
  }
  // Log only on a leadership transition; Tick runs on a loop.
  if (is_leader_.load() != was_leader) {
    if (is_leader_.load())
      spdlog::info("cluster: became leader (node {})", options_.node_id);
    else
      spdlog::info("cluster: no longer leader (node {})", options_.node_id);
  }

  // The leader keeps the shard map in step with membership.
  if (is_leader_.load()) {
    absl::StatusOr<std::vector<NodeInfo>> members = Members();
    if (!members.ok())
      return members.status();
    ShardMap map(options_.virtual_nodes);
    std::vector<std::string> ids;
    ids.reserve(members->size());
    for (const NodeInfo &node : *members)
      ids.push_back(node.id);
    map.SetNodes(std::move(ids));
    const std::string serialized = map.Serialize();
    if (serialized != published_shard_map_) {
      if (absl::Status s = etcd_->Put(ShardMapKey(), serialized, /*lease=*/0);
          !s.ok()) {
        return s;
      }
      published_shard_map_ = serialized;
      spdlog::info("cluster: published shard map for {} node(s)",
                   members->size());
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::optional<std::string>> ClusterManager::CurrentLeader() {
  return etcd_->Get(options_.leader_key);
}

absl::StatusOr<std::vector<NodeInfo>> ClusterManager::Members() {
  const std::string prefix = MembersPrefix();
  absl::StatusOr<std::vector<KeyValue>> kvs = etcd_->GetPrefix(prefix);
  if (!kvs.ok())
    return kvs.status();
  std::vector<NodeInfo> members;
  members.reserve(kvs->size());
  for (const KeyValue &kv : *kvs) {
    members.push_back(NodeInfo{kv.key.substr(prefix.size()), kv.value});
  }
  std::sort(members.begin(), members.end(),
            [](const NodeInfo &a, const NodeInfo &b) { return a.id < b.id; });
  return members;
}

absl::StatusOr<ShardMap> ClusterManager::CurrentShardMap() {
  absl::StatusOr<std::optional<std::string>> text = etcd_->Get(ShardMapKey());
  if (!text.ok())
    return text.status();
  if (text->has_value()) {
    return ShardMap::Parse(**text, options_.virtual_nodes);
  }
  // No published map yet: derive one from current membership.
  absl::StatusOr<std::vector<NodeInfo>> members = Members();
  if (!members.ok())
    return members.status();
  ShardMap map(options_.virtual_nodes);
  std::vector<std::string> ids;
  for (const NodeInfo &node : *members)
    ids.push_back(node.id);
  map.SetNodes(std::move(ids));
  return map;
}

absl::StatusOr<std::uint64_t> ClusterManager::CurrentEpoch() {
  absl::StatusOr<std::optional<std::string>> text = etcd_->Get(EpochKey());
  if (!text.ok())
    return text.status();
  if (!text->has_value())
    return std::uint64_t{0};
  std::uint64_t version = 0;
  if (!absl::SimpleAtoi(**text, &version)) {
    return absl::DataLossError("epoch key does not hold a number");
  }
  return version;
}

absl::Status ClusterManager::AdvanceEpoch(std::uint64_t version) {
  if (!is_leader_.load()) {
    return absl::FailedPreconditionError(
        "only the leader advances the epoch version");
  }
  return etcd_->Put(EpochKey(), absl::StrCat(version), /*lease=*/0);
}

void ClusterManager::Start() {
  if (running_.exchange(true))
    return;
  spdlog::info("cluster: manager started (node {})", options_.node_id);
  worker_ = std::thread([this]() {
    const int period = std::max(1, options_.lease_ttl_seconds / 3);
    while (running_.load()) {
      Tick().IgnoreError();
      for (int i = 0; i < period * 10 && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  });
}

void ClusterManager::Stop() {
  if (!running_.exchange(false)) {
    // Not running; still revoke any lease we hold so we leave promptly.
  }
  if (worker_.joinable())
    worker_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  if (lease_ != 0) {
    spdlog::info("cluster: revoking lease and leaving (node {})",
                 options_.node_id);
    etcd_->LeaseRevoke(lease_).IgnoreError();
    lease_ = 0;
    is_leader_.store(false);
  }
}

} // namespace opaquedb::cluster

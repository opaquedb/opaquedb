#ifndef OPAQUEDB_CLUSTER_SHARD_MAP_H_
#define OPAQUEDB_CLUSTER_SHARD_MAP_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

// A consistent-hash shard map. Each node is placed at several points on a hash
// ring; a shard key is owned by the next node clockwise. A membership change
// moves only the keys between the changed node and its ring neighbours, not the
// whole keyspace. The map is published in etcd as the sorted node list and
// every node rebuilds the identical ring from it, so all nodes agree on
// ownership without sharing more than the membership.

namespace opaquedb::cluster {

// A stable 64-bit hash (FNV-1a) so every node computes the same ring.
std::uint64_t StableHash(const std::string &data);

class ShardMap {
public:
  explicit ShardMap(int virtual_nodes = 128) : virtual_nodes_(virtual_nodes) {}

  // Replaces the node set and rebuilds the ring. Node ids are de-duplicated and
  // ordered, so the ring is independent of insertion order.
  void SetNodes(std::vector<std::string> node_ids);

  const std::vector<std::string> &nodes() const { return nodes_; }
  bool empty() const { return nodes_.empty(); }

  // The node that owns a key hash. Fails only when there are no nodes.
  absl::StatusOr<std::string> OwnerOfHash(std::uint64_t key_hash) const;
  absl::StatusOr<std::string> OwnerOfKey(const std::string &key) const {
    return OwnerOfHash(StableHash(key));
  }

  // The sorted node list as a newline-joined string, the form published in
  // etcd.
  std::string Serialize() const;
  static ShardMap Parse(const std::string &text, int virtual_nodes = 128);

private:
  void Rebuild();

  int virtual_nodes_;
  std::vector<std::string> nodes_;
  std::map<std::uint64_t, std::string> ring_; // ring point -> node id
};

} // namespace opaquedb::cluster

#endif // OPAQUEDB_CLUSTER_SHARD_MAP_H_

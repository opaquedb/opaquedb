#include "opaquedb/cluster/shard_map.h"

#include <algorithm>
#include <set>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"

namespace opaquedb::cluster {

std::uint64_t StableHash(const std::string &data) {
  // FNV-1a, 64-bit. Stable across processes and machines, which is required so
  // every node builds the same ring.
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : data) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

void ShardMap::Rebuild() {
  ring_.clear();
  for (const std::string &node : nodes_) {
    for (int v = 0; v < virtual_nodes_; ++v) {
      ring_[StableHash(absl::StrCat(node, "#", v))] = node;
    }
  }
}

void ShardMap::SetNodes(std::vector<std::string> node_ids) {
  std::set<std::string> unique(node_ids.begin(), node_ids.end());
  nodes_.assign(unique.begin(), unique.end()); // sorted, de-duplicated
  Rebuild();
}

absl::StatusOr<std::string>
ShardMap::OwnerOfHash(std::uint64_t key_hash) const {
  if (ring_.empty()) {
    return absl::FailedPreconditionError("shard map has no nodes");
  }
  // First ring point at or after the key, wrapping to the first point.
  auto it = ring_.lower_bound(key_hash);
  if (it == ring_.end())
    it = ring_.begin();
  return it->second;
}

std::string ShardMap::Serialize() const { return absl::StrJoin(nodes_, "\n"); }

ShardMap ShardMap::Parse(const std::string &text, int virtual_nodes) {
  ShardMap map(virtual_nodes);
  std::vector<std::string> nodes;
  for (absl::string_view line : absl::StrSplit(text, '\n', absl::SkipEmpty())) {
    nodes.emplace_back(line);
  }
  map.SetNodes(std::move(nodes));
  return map;
}

} // namespace opaquedb::cluster

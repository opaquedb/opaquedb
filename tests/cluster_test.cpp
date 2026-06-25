#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "opaquedb/cluster/fake_etcd.h"
#include "opaquedb/cluster/manager.h"
#include "opaquedb/cluster/shard_map.h"

namespace {

using opaquedb::cluster::ClusterManager;
using opaquedb::cluster::ClusterOptions;
using opaquedb::cluster::FakeEtcd;
using opaquedb::cluster::ShardMap;

// ---- FakeEtcd ------------------------------------------------------------

TEST(FakeEtcd, LeaseExpiryDropsKeys) {
  FakeEtcd etcd;
  auto lease = etcd.LeaseGrant(10);
  ASSERT_TRUE(lease.ok());
  ASSERT_TRUE(etcd.Put("/k", "v", *lease).ok());
  ASSERT_TRUE(etcd.Get("/k").ok());
  EXPECT_TRUE(etcd.Get("/k")->has_value());

  etcd.AdvanceTime(5);
  ASSERT_TRUE(etcd.LeaseKeepAlive(*lease).ok());
  etcd.AdvanceTime(6); // 11s total, but kept alive at 5, so alive until 15
  EXPECT_TRUE(etcd.Get("/k")->has_value());

  etcd.AdvanceTime(20); // now well past, no keepalive
  EXPECT_FALSE(etcd.Get("/k")->has_value());
  EXPECT_FALSE(etcd.LeaseKeepAlive(*lease).ok()); // lease gone
}

TEST(FakeEtcd, CreateIfAbsentIsAtomic) {
  FakeEtcd etcd;
  auto lease = etcd.LeaseGrant(10);
  ASSERT_TRUE(lease.ok());
  auto first = etcd.CreateIfAbsent("/leader", "a", *lease);
  ASSERT_TRUE(first.ok());
  EXPECT_TRUE(*first);
  auto second = etcd.CreateIfAbsent("/leader", "b", *lease);
  ASSERT_TRUE(second.ok());
  EXPECT_FALSE(*second); // already held
}

// ---- ShardMap ------------------------------------------------------------

TEST(ShardMap, OwnershipIsStableAndDeterministic) {
  ShardMap a;
  a.SetNodes({"n1", "n2", "n3"});
  ShardMap b;
  b.SetNodes({"n3", "n1", "n2"}); // different insertion order
  // Same nodes -> identical ring -> identical ownership for every key.
  const std::vector<std::string> keys = {"alpha", "bravo", "charlie", "delta"};
  for (const std::string &key : keys) {
    auto owner_a = a.OwnerOfKey(key);
    auto owner_b = b.OwnerOfKey(key);
    ASSERT_TRUE(owner_a.ok());
    ASSERT_TRUE(owner_b.ok());
    EXPECT_EQ(*owner_a, *owner_b);
  }
}

TEST(ShardMap, AllKeysMapToLiveNodes) {
  ShardMap map;
  map.SetNodes({"n1", "n2", "n3"});
  std::set<std::string> live = {"n1", "n2", "n3"};
  for (int i = 0; i < 200; ++i) {
    auto owner = map.OwnerOfKey("key-" + std::to_string(i));
    ASSERT_TRUE(owner.ok());
    EXPECT_TRUE(live.count(*owner)) << *owner;
  }
}

TEST(ShardMap, RemovingANodeMovesFewKeys) {
  ShardMap before;
  before.SetNodes({"n1", "n2", "n3", "n4"});
  ShardMap after;
  after.SetNodes({"n1", "n2", "n3"}); // n4 left
  int moved = 0;
  const int total = 1000;
  for (int i = 0; i < total; ++i) {
    const std::string key = "k" + std::to_string(i);
    if (*before.OwnerOfKey(key) != *after.OwnerOfKey(key))
      ++moved;
  }
  // Only keys that were on n4 should move; far less than half.
  EXPECT_LT(moved, total / 2) << "consistent hashing moved too many keys";
}

TEST(ShardMap, SerializeRoundTrips) {
  ShardMap map;
  map.SetNodes({"n2", "n1"});
  ShardMap parsed = ShardMap::Parse(map.Serialize());
  EXPECT_EQ(parsed.nodes(), (std::vector<std::string>{"n1", "n2"}));
}

TEST(ShardMap, EmptyMapHasNoOwner) {
  ShardMap map;
  EXPECT_FALSE(map.OwnerOfKey("x").ok());
}

// ---- ClusterManager ------------------------------------------------------

ClusterOptions OptsFor(const std::string &id) {
  ClusterOptions o;
  o.node_id = id;
  o.node_address = id + ":50051";
  o.leader_key = "/opaquedb/leader";
  o.lease_ttl_seconds = 10;
  return o;
}

TEST(ClusterManager, SingleNodeBecomesLeaderAndPublishes) {
  FakeEtcd etcd;
  ClusterManager node(&etcd, OptsFor("a"));
  ASSERT_TRUE(node.Tick().ok());
  EXPECT_TRUE(node.is_leader());

  auto members = node.Members();
  ASSERT_TRUE(members.ok());
  ASSERT_EQ(members->size(), 1u);
  EXPECT_EQ((*members)[0].id, "a");
  EXPECT_EQ((*members)[0].address, "a:50051");

  auto map = node.CurrentShardMap();
  ASSERT_TRUE(map.ok());
  EXPECT_EQ(map->nodes(), (std::vector<std::string>{"a"}));
}

TEST(ClusterManager, SecondNodeIsFollowerThenTakesOverOnLeaseExpiry) {
  FakeEtcd etcd;
  ClusterManager a(&etcd, OptsFor("a"));
  ClusterManager b(&etcd, OptsFor("b"));

  ASSERT_TRUE(a.Tick().ok());
  ASSERT_TRUE(b.Tick().ok());
  EXPECT_TRUE(a.is_leader());
  EXPECT_FALSE(b.is_leader());

  // a republishes now that b has joined.
  ASSERT_TRUE(a.Tick().ok());
  auto map = a.CurrentShardMap();
  ASSERT_TRUE(map.ok());
  EXPECT_EQ(map->nodes(), (std::vector<std::string>{"a", "b"}));

  // b keeps its lease alive; a goes silent and its lease lapses.
  etcd.AdvanceTime(5);
  ASSERT_TRUE(b.Tick().ok()); // renews b's lease to t=15
  etcd.AdvanceTime(6);        // t=11: a's lease (granted at 0) has expired
  ASSERT_TRUE(b.Tick().ok());
  EXPECT_TRUE(b.is_leader()) << "b should take over after a's lease expired";
}

TEST(ClusterManager, OnlyLeaderAdvancesEpoch) {
  FakeEtcd etcd;
  ClusterManager a(&etcd, OptsFor("a"));
  ClusterManager b(&etcd, OptsFor("b"));
  ASSERT_TRUE(a.Tick().ok());
  ASSERT_TRUE(b.Tick().ok());

  EXPECT_EQ(*a.CurrentEpoch(), 0u);
  ASSERT_TRUE(a.AdvanceEpoch(7).ok());  // a is leader
  EXPECT_FALSE(b.AdvanceEpoch(8).ok()); // b is not
  EXPECT_EQ(*b.CurrentEpoch(), 7u);     // followers read the pinned epoch
}

} // namespace

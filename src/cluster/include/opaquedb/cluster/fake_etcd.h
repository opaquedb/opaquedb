#ifndef OPAQUEDB_CLUSTER_FAKE_ETCD_H_
#define OPAQUEDB_CLUSTER_FAKE_ETCD_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "opaquedb/cluster/etcd_client.h"

// An in-memory EtcdClient for tests. Lease expiry is driven by a virtual clock
// that a test advances explicitly, so leader handoff and membership churn are
// deterministic with no sleeping. It is thread-safe so the cluster manager's
// background tick can run against it.

namespace opaquedb::cluster {

class FakeEtcd : public EtcdClient {
public:
  absl::StatusOr<LeaseId> LeaseGrant(int ttl_seconds) override;
  absl::Status LeaseKeepAlive(LeaseId lease) override;
  absl::Status LeaseRevoke(LeaseId lease) override;
  absl::Status Put(const std::string &key, const std::string &value,
                   LeaseId lease) override;
  absl::StatusOr<std::optional<std::string>>
  Get(const std::string &key) override;
  absl::StatusOr<std::vector<KeyValue>>
  GetPrefix(const std::string &prefix) override;
  absl::Status Delete(const std::string &key) override;
  absl::StatusOr<bool> CreateIfAbsent(const std::string &key,
                                      const std::string &value,
                                      LeaseId lease) override;

  // Test clock control: move time forward, expiring any lease whose timer ran
  // out, and dropping the keys tied to it.
  void AdvanceTime(int seconds);

private:
  void PurgeExpiredLocked();

  struct Lease {
    int ttl_seconds = 0;
    std::int64_t expires_at = 0;
  };
  struct Entry {
    std::string value;
    LeaseId lease = 0;
  };

  mutable std::mutex mutex_;
  std::int64_t now_ = 0;
  LeaseId next_lease_ = 1;
  std::map<LeaseId, Lease> leases_;
  std::map<std::string, Entry> kv_;
};

} // namespace opaquedb::cluster

#endif // OPAQUEDB_CLUSTER_FAKE_ETCD_H_

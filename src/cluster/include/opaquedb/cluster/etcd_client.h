#ifndef OPAQUEDB_CLUSTER_ETCD_CLIENT_H_
#define OPAQUEDB_CLUSTER_ETCD_CLIENT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

// The EtcdClient adapter. etcd is wrapped behind this narrow interface so it is
// swappable and mockable: the real client uses etcd-cpp-apiv3 (synchronous,
// core only), and an in-memory fake drives the unit tests with no network.
//
// Leader election and membership are built from these primitives (key/value,
// leases, and an atomic create), not from any higher-level campaign helper, so
// the logic is explicit and testable. Push-based watches are a future
// optimization over the periodic reads used now; that does not change this
// interface's contract.

namespace opaquedb::cluster {

using LeaseId = std::int64_t;

struct KeyValue {
  std::string key;
  std::string value;
};

class EtcdClient {
public:
  virtual ~EtcdClient() = default;

  // Grants a lease that expires after ttl seconds unless kept alive.
  virtual absl::StatusOr<LeaseId> LeaseGrant(int ttl_seconds) = 0;
  // Resets a lease's timer. Fails if the lease has already expired or was
  // revoked, which is how a node learns it lost its lease.
  virtual absl::Status LeaseKeepAlive(LeaseId lease) = 0;
  virtual absl::Status LeaseRevoke(LeaseId lease) = 0;

  // Writes a key. lease 0 means no lease (the key persists); a non-zero lease
  // ties the key's lifetime to that lease.
  virtual absl::Status Put(const std::string &key, const std::string &value,
                           LeaseId lease) = 0;
  virtual absl::StatusOr<std::optional<std::string>>
  Get(const std::string &key) = 0;
  virtual absl::StatusOr<std::vector<KeyValue>>
  GetPrefix(const std::string &prefix) = 0;
  virtual absl::Status Delete(const std::string &key) = 0;

  // Creates key with value and lease only if it does not already exist. Returns
  // true when this call created it. This is the compare-and-swap leader
  // election relies on: exactly one caller wins.
  virtual absl::StatusOr<bool> CreateIfAbsent(const std::string &key,
                                              const std::string &value,
                                              LeaseId lease) = 0;
};

} // namespace opaquedb::cluster

#endif // OPAQUEDB_CLUSTER_ETCD_CLIENT_H_

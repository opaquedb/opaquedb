#include "opaquedb/cluster/fake_etcd.h"

#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

namespace opaquedb::cluster {

void FakeEtcd::PurgeExpiredLocked() {
  std::vector<LeaseId> expired;
  for (const auto &[id, lease] : leases_) {
    if (lease.expires_at <= now_)
      expired.push_back(id);
  }
  for (LeaseId id : expired) {
    leases_.erase(id);
    for (auto it = kv_.begin(); it != kv_.end();) {
      if (it->second.lease == id) {
        it = kv_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

absl::StatusOr<LeaseId> FakeEtcd::LeaseGrant(int ttl_seconds) {
  if (ttl_seconds <= 0) {
    return absl::InvalidArgumentError("lease ttl must be positive");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const LeaseId id = next_lease_++;
  leases_[id] = Lease{ttl_seconds, now_ + ttl_seconds};
  return id;
}

absl::Status FakeEtcd::LeaseKeepAlive(LeaseId lease) {
  std::lock_guard<std::mutex> lock(mutex_);
  PurgeExpiredLocked();
  auto it = leases_.find(lease);
  if (it == leases_.end()) {
    return absl::NotFoundError(absl::StrCat("lease ", lease, " expired"));
  }
  it->second.expires_at = now_ + it->second.ttl_seconds;
  return absl::OkStatus();
}

absl::Status FakeEtcd::LeaseRevoke(LeaseId lease) {
  std::lock_guard<std::mutex> lock(mutex_);
  leases_.erase(lease);
  for (auto it = kv_.begin(); it != kv_.end();) {
    if (it->second.lease == lease) {
      it = kv_.erase(it);
    } else {
      ++it;
    }
  }
  return absl::OkStatus();
}

absl::Status FakeEtcd::Put(const std::string &key, const std::string &value,
                           LeaseId lease) {
  std::lock_guard<std::mutex> lock(mutex_);
  PurgeExpiredLocked();
  if (lease != 0 && leases_.find(lease) == leases_.end()) {
    return absl::NotFoundError(absl::StrCat("lease ", lease, " expired"));
  }
  kv_[key] = Entry{value, lease};
  return absl::OkStatus();
}

absl::StatusOr<std::optional<std::string>>
FakeEtcd::Get(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);
  PurgeExpiredLocked();
  auto it = kv_.find(key);
  if (it == kv_.end())
    return std::optional<std::string>{};
  return std::optional<std::string>(it->second.value);
}

absl::StatusOr<std::vector<KeyValue>>
FakeEtcd::GetPrefix(const std::string &prefix) {
  std::lock_guard<std::mutex> lock(mutex_);
  PurgeExpiredLocked();
  std::vector<KeyValue> out;
  for (const auto &[key, entry] : kv_) {
    if (absl::StartsWith(key, prefix))
      out.push_back(KeyValue{key, entry.value});
  }
  return out;
}

absl::Status FakeEtcd::Delete(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);
  kv_.erase(key);
  return absl::OkStatus();
}

absl::StatusOr<bool> FakeEtcd::CreateIfAbsent(const std::string &key,
                                              const std::string &value,
                                              LeaseId lease) {
  std::lock_guard<std::mutex> lock(mutex_);
  PurgeExpiredLocked();
  if (kv_.find(key) != kv_.end())
    return false;
  if (lease != 0 && leases_.find(lease) == leases_.end()) {
    return absl::NotFoundError(absl::StrCat("lease ", lease, " expired"));
  }
  kv_[key] = Entry{value, lease};
  return true;
}

void FakeEtcd::AdvanceTime(int seconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  now_ += seconds;
  PurgeExpiredLocked();
}

} // namespace opaquedb::cluster

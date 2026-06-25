#include "opaquedb/cluster/real_etcd_client.h"

#ifdef OPAQUEDB_HAVE_ETCD

#include <exception>
#include <map>
#include <mutex>
#include <utility>

#include "absl/strings/str_cat.h"
#include "etcd/KeepAlive.hpp"
#include "etcd/Response.hpp"
#include "etcd/SyncClient.hpp"
#include "etcd/Value.hpp"
#include "spdlog/spdlog.h"

namespace opaquedb::cluster {
namespace {

// etcdv3 "key not found" error code from etcd-cpp-apiv3.
constexpr int kKeyNotFound = 100;

// Builds the underlying SyncClient for the chosen auth. TLS wins if both TLS and
// password settings are present because the simple client does not combine them
// in one channel. Never logs the password.
std::unique_ptr<etcd::SyncClient> MakeSyncClient(const std::string &endpoint,
                                                 const EtcdAuth &auth) {
  if (!auth.ca_cert.empty()) {
    spdlog::info("etcd: connecting to {} over TLS{}", endpoint,
                 auth.client_cert.empty() ? "" : " with client certificate");
    return std::make_unique<etcd::SyncClient>(endpoint, auth.ca_cert,
                                              auth.client_cert, auth.client_key,
                                              auth.tls_name);
  }
  if (!auth.username.empty()) {
    spdlog::info("etcd: connecting to {} with password auth as user {}",
                 endpoint, auth.username);
    return std::make_unique<etcd::SyncClient>(endpoint, auth.username,
                                              auth.password);
  }
  spdlog::info("etcd: connecting to {} without authentication", endpoint);
  return std::make_unique<etcd::SyncClient>(endpoint);
}

class RealEtcdClient : public EtcdClient {
public:
  RealEtcdClient(const std::string &endpoint, const EtcdAuth &auth)
      : client_(MakeSyncClient(endpoint, auth)) {}

  absl::StatusOr<LeaseId> LeaseGrant(int ttl_seconds) override {
    try {
      // leasekeepalive grants a lease and renews it in the background for as
      // long as we hold the returned handle.
      std::shared_ptr<etcd::KeepAlive> keep =
          client_->leasekeepalive(ttl_seconds);
      const LeaseId lease = keep->Lease();
      std::lock_guard<std::mutex> lock(mutex_);
      keep_alives_[lease] = std::move(keep);
      return lease;
    } catch (const std::exception &e) {
      return absl::UnavailableError(
          absl::StrCat("etcd lease grant failed: ", e.what()));
    }
  }

  absl::Status LeaseKeepAlive(LeaseId lease) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = keep_alives_.find(lease);
    if (it == keep_alives_.end()) {
      return absl::NotFoundError(absl::StrCat("lease ", lease, " not held"));
    }
    // Surface a background renewal failure as a lost lease.
    try {
      it->second->Check();
    } catch (const std::exception &e) {
      keep_alives_.erase(it);
      return absl::NotFoundError(
          absl::StrCat("lease ", lease, " lapsed: ", e.what()));
    }
    return absl::OkStatus();
  }

  absl::Status LeaseRevoke(LeaseId lease) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      keep_alives_.erase(lease); // stops the background renewer
    }
    try {
      client_->leaserevoke(lease);
    } catch (const std::exception &e) {
      return absl::UnavailableError(
          absl::StrCat("etcd lease revoke failed: ", e.what()));
    }
    return absl::OkStatus();
  }

  absl::Status Put(const std::string &key, const std::string &value,
                   LeaseId lease) override {
    try {
      etcd::Response resp = lease == 0 ? client_->put(key, value)
                                       : client_->put(key, value, lease);
      if (!resp.is_ok()) {
        return absl::UnavailableError(
            absl::StrCat("etcd put failed: ", resp.error_message()));
      }
      return absl::OkStatus();
    } catch (const std::exception &e) {
      return absl::UnavailableError(absl::StrCat("etcd put threw: ", e.what()));
    }
  }

  absl::StatusOr<std::optional<std::string>>
  Get(const std::string &key) override {
    try {
      etcd::Response resp = client_->get(key);
      if (resp.is_ok())
        return std::optional<std::string>(resp.value().as_string());
      if (resp.error_code() == kKeyNotFound) {
        return std::optional<std::string>{};
      }
      return absl::UnavailableError(
          absl::StrCat("etcd get failed: ", resp.error_message()));
    } catch (const std::exception &e) {
      return absl::UnavailableError(absl::StrCat("etcd get threw: ", e.what()));
    }
  }

  absl::StatusOr<std::vector<KeyValue>>
  GetPrefix(const std::string &prefix) override {
    try {
      etcd::Response resp = client_->ls(prefix);
      if (!resp.is_ok()) {
        return absl::UnavailableError(
            absl::StrCat("etcd ls failed: ", resp.error_message()));
      }
      std::vector<KeyValue> out;
      for (std::size_t i = 0; i < resp.keys().size(); ++i) {
        const int idx = static_cast<int>(i);
        out.push_back(KeyValue{resp.key(idx), resp.value(idx).as_string()});
      }
      return out;
    } catch (const std::exception &e) {
      return absl::UnavailableError(absl::StrCat("etcd ls threw: ", e.what()));
    }
  }

  absl::Status Delete(const std::string &key) override {
    try {
      client_->rm(key);
      return absl::OkStatus();
    } catch (const std::exception &e) {
      return absl::UnavailableError(absl::StrCat("etcd rm threw: ", e.what()));
    }
  }

  absl::StatusOr<bool> CreateIfAbsent(const std::string &key,
                                      const std::string &value,
                                      LeaseId lease) override {
    try {
      // add() is an atomic create that fails if the key already exists, which
      // is the compare-and-swap leader election needs.
      etcd::Response resp = client_->add(key, value, lease);
      return resp.is_ok();
    } catch (const std::exception &e) {
      return absl::UnavailableError(absl::StrCat("etcd add threw: ", e.what()));
    }
  }

private:
  std::unique_ptr<etcd::SyncClient> client_;
  std::mutex mutex_;
  std::map<LeaseId, std::shared_ptr<etcd::KeepAlive>> keep_alives_;
};

} // namespace

absl::StatusOr<std::unique_ptr<EtcdClient>>
MakeRealEtcdClient(const std::vector<std::string> &endpoints,
                   const EtcdAuth &auth) {
  if (endpoints.empty()) {
    return absl::InvalidArgumentError("no etcd endpoints configured");
  }
  try {
    return std::unique_ptr<EtcdClient>(
        std::make_unique<RealEtcdClient>(endpoints.front(), auth));
  } catch (const std::exception &e) {
    return absl::UnavailableError(
        absl::StrCat("cannot connect to etcd: ", e.what()));
  }
}

} // namespace opaquedb::cluster

#endif // OPAQUEDB_HAVE_ETCD

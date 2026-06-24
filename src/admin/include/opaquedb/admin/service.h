#ifndef OPAQUEDB_ADMIN_SERVICE_H_
#define OPAQUEDB_ADMIN_SERVICE_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/admin/keyring_store.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/storage/repository.h"

// The AdminService facade composes the management operations: schema
// inspection, epoch publish/list/rollback, keyring inspection, and node status.
// It is transport agnostic, so the gRPC adapter and a future HTTP/JSON adapter
// (the web UI) are thin clients over the same logic, and so the CLI can call it
// in process for local management. Management logic lives here once.
//
// In the single-node build the facade acts on the local repository. The
// distributed build routes write operations to the leader; that wraps these
// same operations without changing them.

namespace opaquedb::admin {

class AdminService {
public:
  AdminService(config::Config config, storage::EpochRepository *repo,
               KeyringStore *keyring)
      : config_(std::move(config)), repo_(repo), keyring_(keyring) {}

  struct NodeStatus {
    std::string node_id;
    std::string role; // standalone for now; leader/follower with cluster
    bool has_epoch = false;
    std::uint64_t epoch_version = 0;
    std::uint64_t row_count = 0;
    std::vector<std::string> backends;
    std::string auth_mode;
  };

  absl::StatusOr<NodeStatus> GetStatus() const;

  // The schema of the current epoch.
  absl::StatusOr<core::Schema> InspectSchema() const;

  absl::StatusOr<std::vector<std::uint64_t>> ListEpochs() const;
  absl::Status RollbackEpoch(std::uint64_t version);

  // Publishes a staging epoch at the next version and returns that version.
  // After local publish succeeds, if a post-publish hook is set it is called
  // with the new version (best-effort; used by cluster leader to advance the
  // global epoch for snapshot consistency).
  absl::StatusOr<std::uint64_t> Publish(const storage::StagingEpoch &staging);

  using PostPublishHook =
      std::function<absl::Status(std::uint64_t epoch_version)>;
  void SetPostPublishHook(PostPublishHook hook);

  // The ids of clients that have registered keys.
  std::vector<std::string> ListPrincipals() const;

  const config::Config &config() const { return config_; }

private:
  config::Config config_;
  storage::EpochRepository *repo_;
  KeyringStore *keyring_;
  PostPublishHook post_publish_hook_;
};

} // namespace opaquedb::admin

#endif // OPAQUEDB_ADMIN_SERVICE_H_

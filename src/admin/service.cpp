#include "opaquedb/admin/service.h"

#include <memory>
#include <utility>

#include "opaquedb/backend/registry.h"
#include "opaquedb/config/loader.h"

namespace opaquedb::admin {

absl::StatusOr<AdminService::NodeStatus> AdminService::GetStatus() const {
  NodeStatus status;
  status.node_id = config_.node.id;
  status.role = "standalone"; // cluster roles arrive with the cluster module
  status.auth_mode = config::ToString(config_.auth.mode);
  for (const backend::BackendCapabilities &caps :
       backend::BackendRegistry::instance().list()) {
    status.backends.push_back(caps.name);
  }
  if (absl::StatusOr<std::uint64_t> version = repo_->CurrentVersion();
      version.ok()) {
    status.has_epoch = true;
    status.epoch_version = *version;
    if (absl::StatusOr<std::unique_ptr<storage::EpochReader>> reader =
            repo_->OpenCurrent();
        reader.ok()) {
      status.row_count = (*reader)->row_count();
    }
  }
  return status;
}

absl::StatusOr<core::Schema> AdminService::InspectSchema() const {
  absl::StatusOr<std::unique_ptr<storage::EpochReader>> reader =
      repo_->OpenCurrent();
  if (!reader.ok())
    return reader.status();
  return (*reader)->manifest().schema;
}

absl::StatusOr<std::vector<std::uint64_t>> AdminService::ListEpochs() const {
  return repo_->ListEpochs();
}

absl::Status AdminService::RollbackEpoch(std::uint64_t version) {
  return repo_->Rollback(version);
}

absl::StatusOr<std::uint64_t>
AdminService::Publish(const storage::StagingEpoch &staging) {
  std::uint64_t next = 1;
  if (absl::StatusOr<std::uint64_t> current = repo_->CurrentVersion();
      current.ok()) {
    next = *current + 1;
  }
  if (absl::Status s = repo_->Publish(staging, next); !s.ok())
    return s;
  if (post_publish_hook_) {
    // Best effort: used in cluster to advance the global snapshot epoch.
    // Non-leaders or non-clustered will ignore/return ok from the hook.
    (void)post_publish_hook_(next);
  }
  return next;
}

void AdminService::SetPostPublishHook(PostPublishHook hook) {
  post_publish_hook_ = std::move(hook);
}

std::vector<std::string> AdminService::ListPrincipals() const {
  return keyring_->ClientIds();
}

} // namespace opaquedb::admin

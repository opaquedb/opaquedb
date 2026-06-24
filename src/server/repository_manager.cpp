#include "opaquedb/server/repository_manager.h"

#include <utility>

namespace opaquedb::server {

absl::StatusOr<storage::EpochRepository *>
RepositoryManager::Get(const core::TableId &id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = repos_.find(id);
  if (it != repos_.end())
    return it->second.get();

  absl::StatusOr<std::unique_ptr<storage::LocalEpochRepository>> repo =
      storage::LocalEpochRepository::Open(
          config_.EpochRootFor(id.database, id.table));
  if (!repo.ok())
    return repo.status();
  storage::EpochRepository *ptr = repo->get();
  repos_.emplace(id, *std::move(repo));
  return ptr;
}

storage::Catalog RepositoryManager::catalog() const {
  return storage::Catalog(config_.DatabasesDir());
}

} // namespace opaquedb::server

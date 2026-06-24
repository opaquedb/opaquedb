#ifndef OPAQUEDB_SERVER_REPOSITORY_MANAGER_H_
#define OPAQUEDB_SERVER_REPOSITORY_MANAGER_H_

#include <map>
#include <memory>
#include <mutex>

#include "absl/status/statusor.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/table_id.h"
#include "opaquedb/storage/catalog.h"
#include "opaquedb/storage/repository.h"

// Owns one repository per (database, table) and opens them on demand. A node
// serves many tables, so the engine asks the manager for the repository that
// holds a given table; the manager opens it at the per-table root the config
// resolves (data_dir/db/<database>/<table>/epochs) and caches it. Lookups are
// thread-safe because queries run concurrently.

namespace opaquedb::server {

class RepositoryManager {
public:
  explicit RepositoryManager(config::Config config)
      : config_(std::move(config)) {}

  // The repository for a table, opening and caching it on first use. The
  // directory is created if absent, so a query for a table with no data yet
  // sees an empty repository (CurrentVersion returns NotFound) rather than an
  // error.
  absl::StatusOr<storage::EpochRepository *> Get(const core::TableId &id);

  // Enumerates the databases and tables present on disk.
  storage::Catalog catalog() const;

private:
  config::Config config_;
  std::mutex mu_;
  std::map<core::TableId, std::unique_ptr<storage::EpochRepository>> repos_;
};

} // namespace opaquedb::server

#endif // OPAQUEDB_SERVER_REPOSITORY_MANAGER_H_

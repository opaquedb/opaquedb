#ifndef OPAQUEDB_STORAGE_CATALOG_H_
#define OPAQUEDB_STORAGE_CATALOG_H_

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "opaquedb/core/table_id.h"

// The catalog enumerates the databases and tables that exist on disk. Tables
// live under <base>/db/<database>/<table>/, so the catalog is a two-level
// directory scan. It does not open epochs or read manifests; it only reports
// which (database, table) pairs are present. Admin uses it for ListDatabases
// and ListTables.

namespace opaquedb::storage {

class Catalog {
public:
  // `db_base` is the directory that holds the per-database subtrees, i.e.
  // <data_dir>/db. A base that does not exist yet means no tables, which is not
  // an error.
  explicit Catalog(std::string db_base) : db_base_(std::move(db_base)) {}

  // Every table present, sorted by (database, table). Empty if the base is
  // absent.
  absl::StatusOr<std::vector<core::TableId>> ListTables() const;

  // Every database that holds at least one table, sorted and de-duplicated.
  absl::StatusOr<std::vector<std::string>> ListDatabases() const;

private:
  std::string db_base_;
};

} // namespace opaquedb::storage

#endif // OPAQUEDB_STORAGE_CATALOG_H_

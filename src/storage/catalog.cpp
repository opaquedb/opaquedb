#include "opaquedb/storage/catalog.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "absl/strings/str_cat.h"

namespace opaquedb::storage {
namespace fs = std::filesystem;

absl::StatusOr<std::vector<core::TableId>> Catalog::ListTables() const {
  std::vector<core::TableId> tables;
  std::error_code ec;
  if (!fs::exists(db_base_, ec) || ec)
    return tables; // no databases created yet

  for (const fs::directory_entry &db_entry :
       fs::directory_iterator(db_base_, ec)) {
    if (ec)
      return absl::InternalError(absl::StrCat("catalog: cannot scan '",
                                              db_base_, "': ", ec.message()));
    if (!db_entry.is_directory())
      continue;
    const std::string database = db_entry.path().filename().string();
    for (const fs::directory_entry &table_entry :
         fs::directory_iterator(db_entry.path(), ec)) {
      if (ec)
        return absl::InternalError(absl::StrCat("catalog: cannot scan '",
                                                db_entry.path().string(),
                                                "': ", ec.message()));
      if (!table_entry.is_directory())
        continue;
      tables.push_back(
          core::TableId{database, table_entry.path().filename().string()});
    }
  }
  std::sort(tables.begin(), tables.end());
  return tables;
}

absl::StatusOr<std::vector<std::string>> Catalog::ListDatabases() const {
  absl::StatusOr<std::vector<core::TableId>> tables = ListTables();
  if (!tables.ok())
    return tables.status();
  std::vector<std::string> dbs;
  for (const core::TableId &id : *tables) {
    if (dbs.empty() || dbs.back() != id.database)
      dbs.push_back(id.database);
  }
  // ListTables is sorted by (database, table), so equal databases are adjacent
  // and the guard above de-duplicates them.
  return dbs;
}

} // namespace opaquedb::storage

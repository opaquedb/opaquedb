#ifndef OPAQUEDB_CORE_TABLE_ID_H_
#define OPAQUEDB_CORE_TABLE_ID_H_

#include <string>
#include <tuple>

// The fully qualified identity of a table: a database name and a table name. A
// cluster holds many databases, each holding many tables. The database defaults
// to "default" so a single-database deployment never has to name it. This lives
// in core because storage, the engine, admin, and the CLI all route by it.

namespace opaquedb::core {

inline constexpr char kDefaultDatabase[] = "default";

struct TableId {
  std::string database = kDefaultDatabase;
  std::string table;

  // A stable "database.table" rendering, used for shard-key namespacing, log
  // lines, and map keys.
  std::string ToString() const { return database + "." + table; }

  bool operator==(const TableId &other) const {
    return database == other.database && table == other.table;
  }
  bool operator<(const TableId &other) const {
    return std::tie(database, table) < std::tie(other.database, other.table);
  }
};

} // namespace opaquedb::core

#endif // OPAQUEDB_CORE_TABLE_ID_H_

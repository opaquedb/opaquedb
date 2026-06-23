#ifndef OPAQUEDB_SQL_DDL_H_
#define OPAQUEDB_SQL_DDL_H_

#include <string_view>

#include "absl/status/statusor.h"
#include "opaquedb/core/schema.h"

// Parses a CREATE TABLE statement into a core::Schema. This is how a user
// declares a table: its columns, their types, and which column is the match
// key. The grammar:
//
//   CREATE TABLE name (
//     col TYPE [KEY],
//     ...
//   );
//
// TYPE is one of INT, REAL, or TEXT (case-insensitive). Exactly one column is
// marked KEY; it is the match key, must be INT, and is encoded as a constant-
// weight codeword. Every other column is payload, returned but never matched. A
// trailing semicolon is optional. The reference backend matches on equality, so
// the key column gets the EQ encoding and the rest get RAW.

namespace opaquedb::sql {

absl::StatusOr<core::Schema> ParseCreateTable(std::string_view ddl);

} // namespace opaquedb::sql

#endif // OPAQUEDB_SQL_DDL_H_

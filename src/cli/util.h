#ifndef OPAQUEDB_CLI_UTIL_H_
#define OPAQUEDB_CLI_UTIL_H_

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "opaquedb/cli/command.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/sql/ast.h"

// Helpers shared by every command, so config loading, logging setup, and file
// reading live in one place instead of being repeated per command.

namespace opaquedb::client {
class QueryClient;
}

namespace opaquedb::cli {

// Resolves config from the global options and initializes logging from it, so
// every command logs through the one configured logger. Returns the config or
// the first error.
absl::StatusOr<config::Config> LoadConfig(const GlobalOptions &globals,
                                          bool file_optional = true);

// Reads a whole file into a string.
absl::StatusOr<std::string> ReadFile(const std::string &path);

// Trims trailing NUL padding from a fixed-size record for raw display.
std::string RenderRaw(const std::vector<std::uint8_t> &record);

// Renders one record as "name=value" pairs. If projection is non-empty, only
// those columns are shown, in projection order; otherwise every payload column
// is shown. A projected column that is not a payload column (the key, or
// unknown) is skipped.
std::string RenderWithSchema(const core::Schema &schema,
                             const std::vector<std::uint8_t> &record,
                             const std::vector<std::string> &projection = {});

// Renders an aligned text table: a header row, a dashed separator, then the
// data rows, columns separated by '|' and padded to the widest cell. With no
// data rows only the header and separator are returned. headers and every row
// must have the same length.
std::string RenderTable(const std::vector<std::string> &headers,
                        const std::vector<std::vector<std::string>> &rows);

// The header row and string cells of a result table, ready for RenderTable.
struct TableData {
  std::vector<std::string> headers;
  std::vector<std::vector<std::string>> rows;
};

// Decodes payload records against the schema and applies the public,
// client-side presentation controls: column projection (with aliases),
// DISTINCT, ORDER BY, then the OFFSET/LIMIT window. The result is ready to hand
// to RenderTable. records are raw payload bytes (from Query or Scan). statement
// carries the projection, aliases, distinct flag, order_by terms, and
// limit/offset. Returns an error if an ORDER BY column is not among the
// returned columns.
absl::StatusOr<TableData>
BuildResultTable(const core::Schema &schema,
                 const std::vector<std::vector<std::uint8_t>> &records,
                 const sql::SelectStatement &statement);

// Runs one parsed SELECT end to end and writes the result to `out`: a COUNT(*)
// number, a full-scan table (no WHERE), or a matched-row table. Handles
// DISTINCT, ORDER BY, and LIMIT/OFFSET over the decoded rows. schema_cache, if
// given, memoizes the per-table schema by "database.table". Returns the first
// error; the caller decides how to report it. Data output (rows, counts,
// warnings) goes to `out`.
absl::Status RunSelect(client::QueryClient &client,
                       const std::string &client_id, const std::string &sql,
                       std::uint64_t value, const std::string &backend,
                       const std::string &database, std::ostream &out,
                       std::map<std::string, core::Schema> *schema_cache);

} // namespace opaquedb::cli

#endif // OPAQUEDB_CLI_UTIL_H_

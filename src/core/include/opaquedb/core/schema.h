#ifndef OPAQUEDB_CORE_SCHEMA_H_
#define OPAQUEDB_CORE_SCHEMA_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

// The table schema and per-column encoding. The schema is the single source of
// truth for what columns exist and which operators are legal on each. It lives
// in core because the planner, storage, and backends all read it; it is
// persisted in the epoch manifest by storage.
//
// The encoding decides how a value is matched. EQ is implemented now. The
// others are declared so the planner can already route to them; adding one is
// filling in evaluation, not changing the schema model.

namespace opaquedb::core {

enum class ColumnEncoding {
  kEq,     // the primary match key: equality matched and the shard key, not
           // stored in payload
  kIndex,  // a secondary match key: equality matched like kEq, but also stored
           // in payload so it is returnable; it does not shard the data
  kPrefix, // positional encoding for startswith (stub)
  kRange,  // ordered bins for approximate range (stub)
  kRaw,    // payload only, never matched, used for returned columns
};

// Whether a column is equality matchable. The primary key (kEq) and secondary
// indexes (kIndex) are; their values are encoded into match segments. This is
// the single predicate that decides who gets a packed match sub-key.
bool IsSearchable(ColumnEncoding encoding);

// The value type of a column. The match key is mapped to a codeword: an INT
// bijectively, a TEXT through a hash. Payload columns carry typed values that
// the client decodes for display.
enum class ColumnType {
  kInt,  // 64-bit signed integer
  kReal, // double-precision float
  kText, // UTF-8 text
};

std::string ToString(ColumnEncoding encoding);
absl::StatusOr<ColumnEncoding> ParseColumnEncoding(std::string_view text);

std::string ToString(ColumnType type);
absl::StatusOr<ColumnType> ParseColumnType(std::string_view text);

struct Column {
  std::string name;
  ColumnEncoding encoding = ColumnEncoding::kRaw;
  ColumnType type = ColumnType::kText;
};

// A table: a name and an ordered list of columns. Column order is the on-disk
// order. The schema carries no data, only structure.
class Schema {
public:
  Schema() = default;
  Schema(std::string table, std::vector<Column> columns)
      : table_(std::move(table)), columns_(std::move(columns)) {}

  const std::string &table() const { return table_; }
  const std::vector<Column> &columns() const { return columns_; }

  // Index of the named column, or nullopt if absent.
  std::optional<std::size_t> IndexOf(std::string_view name) const;

  // Number of searchable columns (kEq plus every kIndex). The match segment
  // packs one key per searchable column, so this is the segment's key count.
  std::size_t SearchableCount() const;

  // Position of a column among the searchable columns, in schema order, or
  // nullopt if the column is out of range or not searchable. This is the index
  // of its packed sub-key inside a match record.
  std::optional<std::size_t> SearchableRank(std::size_t column_index) const;

  // Whether a column with this encoding can be matched by the given... left to
  // the planner. Helpers here are structural only.
  bool IsMatchable(std::string_view column) const;

  // Validates structure: non-empty table name, at least one column, unique
  // non-empty column names. Does not judge which encodings are implemented;
  // that is the planner's job.
  absl::Status Validate() const;

private:
  std::string table_;
  std::vector<Column> columns_;
};

} // namespace opaquedb::core

#endif // OPAQUEDB_CORE_SCHEMA_H_

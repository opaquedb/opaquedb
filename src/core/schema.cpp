#include "opaquedb/core/schema.h"

#include <set>
#include <string_view>

#include "absl/strings/str_cat.h"

namespace opaquedb::core {

std::string ToString(ColumnEncoding encoding) {
  switch (encoding) {
  case ColumnEncoding::kEq:
    return "eq";
  case ColumnEncoding::kIndex:
    return "index";
  case ColumnEncoding::kPrefix:
    return "prefix";
  case ColumnEncoding::kRange:
    return "range";
  case ColumnEncoding::kRaw:
    return "raw";
  }
  return "unknown";
}

absl::StatusOr<ColumnEncoding> ParseColumnEncoding(std::string_view text) {
  if (text == "eq")
    return ColumnEncoding::kEq;
  if (text == "index")
    return ColumnEncoding::kIndex;
  if (text == "prefix")
    return ColumnEncoding::kPrefix;
  if (text == "range")
    return ColumnEncoding::kRange;
  if (text == "raw")
    return ColumnEncoding::kRaw;
  return absl::InvalidArgumentError(
      absl::StrCat("unknown column encoding '", text, "'"));
}

bool IsSearchable(ColumnEncoding encoding) {
  return encoding == ColumnEncoding::kEq || encoding == ColumnEncoding::kIndex;
}

std::string ToString(ColumnType type) {
  switch (type) {
  case ColumnType::kInt:
    return "int";
  case ColumnType::kReal:
    return "real";
  case ColumnType::kText:
    return "text";
  }
  return "unknown";
}

absl::StatusOr<ColumnType> ParseColumnType(std::string_view text) {
  if (text == "int")
    return ColumnType::kInt;
  if (text == "real")
    return ColumnType::kReal;
  if (text == "text")
    return ColumnType::kText;
  return absl::InvalidArgumentError(
      absl::StrCat("unknown column type '", text, "'"));
}

std::optional<std::size_t> Schema::IndexOf(std::string_view name) const {
  for (std::size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].name == name)
      return i;
  }
  return std::nullopt;
}

std::size_t Schema::SearchableCount() const {
  std::size_t count = 0;
  for (const Column &column : columns_) {
    if (IsSearchable(column.encoding))
      ++count;
  }
  return count;
}

std::optional<std::size_t>
Schema::SearchableRank(std::size_t column_index) const {
  if (column_index >= columns_.size() ||
      !IsSearchable(columns_[column_index].encoding)) {
    return std::nullopt;
  }
  std::size_t rank = 0;
  for (std::size_t i = 0; i < column_index; ++i) {
    if (IsSearchable(columns_[i].encoding))
      ++rank;
  }
  return rank;
}

bool Schema::IsMatchable(std::string_view column) const {
  std::optional<std::size_t> idx = IndexOf(column);
  if (!idx)
    return false;
  return columns_[*idx].encoding != ColumnEncoding::kRaw;
}

absl::Status Schema::Validate() const {
  if (table_.empty()) {
    return absl::InvalidArgumentError("schema: table name is empty");
  }
  if (columns_.empty()) {
    return absl::InvalidArgumentError("schema: table has no columns");
  }
  std::set<std::string_view> seen;
  std::size_t match_keys = 0;
  for (const Column &column : columns_) {
    if (column.name.empty()) {
      return absl::InvalidArgumentError("schema: a column name is empty");
    }
    if (!seen.insert(column.name).second) {
      return absl::InvalidArgumentError(
          absl::StrCat("schema: duplicate column name '", column.name, "'"));
    }
    if (column.encoding == ColumnEncoding::kEq) {
      ++match_keys;
    }
    if (IsSearchable(column.encoding)) {
      // A match key is mapped to a codeword. INT maps bijectively (exact
      // match); TEXT maps through a hash (a candidate match the client
      // verifies). REAL has no well defined equality here. This holds for the
      // primary key (kEq) and every secondary index (kIndex).
      if (column.type == ColumnType::kReal) {
        return absl::InvalidArgumentError(
            absl::StrCat("schema: match column '", column.name,
                         "' must be an INT or TEXT column, not REAL"));
      }
    }
  }
  if (match_keys != 1) {
    return absl::InvalidArgumentError(absl::StrCat(
        "schema: a table needs exactly one match key column, found ",
        match_keys));
  }
  return absl::OkStatus();
}

} // namespace opaquedb::core

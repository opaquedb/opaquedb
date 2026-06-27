#ifndef OPAQUEDB_SQL_TOKENIZER_H_
#define OPAQUEDB_SQL_TOKENIZER_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

// The tokenizer for the SQL subset. Keywords are case-insensitive; identifiers
// keep their case so they match schema column names exactly. The query value is
// a bound parameter (:name); string and number literals are tokenized only so
// the parser can reject them with a clear message where a value is expected.
//
// Input is treated as untrusted. The length, token count, and identifier length
// are bounded so a hostile template cannot drive unbounded work or allocation.

namespace opaquedb::sql {

inline constexpr std::size_t kMaxSqlLength = 64 * 1024;
inline constexpr std::size_t kMaxTokens = 4096;
inline constexpr std::size_t kMaxIdentifierLength = 256;

enum class TokenType {
  kSelect,
  kCount, // COUNT, only as the aggregate COUNT(*)
  kFrom,
  kWhere,
  kAnd,
  kOr,
  kIn,
  kLike,
  kBetween,
  kLimit,
  kOffset,
  kCreate,
  kTable,
  kKey,
  kIndex,
  kIdentifier,
  kParameter,     // ':name', text holds the name without the colon
  kStringLiteral, // 'text', for rejection where a value is expected
  kNumberLiteral, // 123, for rejection where a value is expected
  kComma,
  kStar, // *, the "all columns" projection in SELECT
  kLParen,
  kRParen,
  kEq, // =
  kNe, // <> or !=
  kLt, // <
  kLe, // <=
  kGt, // >
  kGe, // >=
  kEnd,
};

struct Token {
  TokenType type;
  std::string text; // identifier, parameter name, or literal contents
  std::size_t pos;  // byte offset in the input, for error messages
};

absl::StatusOr<std::vector<Token>> Tokenize(std::string_view sql);

std::string ToString(TokenType type);

} // namespace opaquedb::sql

#endif // OPAQUEDB_SQL_TOKENIZER_H_

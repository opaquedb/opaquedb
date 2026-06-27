#ifndef OPAQUEDB_SQL_PARSER_H_
#define OPAQUEDB_SQL_PARSER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/sql/ast.h"

// A small hand-written recursive-descent parser for the SQL subset. It parses
// the full grammar, including the operators that are not yet evaluable, into
// the AST. Whether a parsed statement can be executed is decided later by the
// plan builder and the planner, not here; a syntactically valid IN or BETWEEN
// parses without error.
//
// The grammar:
//   statement  := SELECT [DISTINCT] projection FROM identifier
//                 [WHERE predicate] [ORDER BY order_list] [LIMIT n] [OFFSET m]
//   projection := '*' | COUNT '(' '*' ')' | proj_col (',' proj_col)*
//   proj_col   := identifier [AS identifier]
//   order_list := identifier [ASC | DESC] (',' identifier [ASC | DESC])*
//   predicate  := or_term (OR or_term)*
//   or_term    := and_term (AND and_term)*
//   and_term   := identifier comparison
//   comparison := ('=' | '<>' | '<' | '<=' | '>' | '>=') parameter
//               | IN '(' parameter (',' parameter)* ')'
//               | LIKE parameter
//               | BETWEEN parameter AND parameter
// WHERE is optional: a SELECT with no WHERE is a full table scan. The WHERE
// value is a bound parameter or an inline literal. A literal is the secret
// value, so the client strips it before sending (see PrepareClientQuery).
// DISTINCT, ORDER BY, LIMIT, and OFFSET are public and applied client-side over
// the decoded rows.

namespace opaquedb::sql {

// The default LIMIT when a SELECT carries no LIMIT clause. Applied client-side
// over the decoded rows. It is bounded by crypto.result_buckets (default 16):
// a match query returns at most result_buckets clean rows per query, so a
// default above that gains nothing without also raising result_buckets.
inline constexpr std::uint64_t kDefaultSelectLimit = 10;

absl::StatusOr<SelectStatement> Parse(std::string_view sql);

// The server-facing form of a client's query: a template with no inline
// literal, plus the literal value (if the original had one) for the client to
// encrypt. If the input already uses a bound parameter, the template is
// returned unchanged and literal is nullopt. Run on the client so a literal
// value never crosses the wire.
struct PreparedQuery {
  std::string sql_template; // parameterized, safe to send
  // The literal value(s) lifted out of the template, in left-to-right order, to
  // be encrypted by the client. Empty when the template already used bound
  // parameters. A single equality lifts one; IN / same-column OR lifts one per
  // listed value (rewritten to :v0, :v1, ...).
  std::vector<core::Value> literals;
  bool count = false; // the statement is SELECT COUNT(*)
  // The LIMIT/OFFSET from the template, so the client knows how many result
  // buckets to decode and from where. Unset limit means the default of 1.
  std::optional<std::uint64_t> limit;
  std::optional<std::uint64_t> offset;
};

absl::StatusOr<PreparedQuery> PrepareClientQuery(std::string_view sql);

} // namespace opaquedb::sql

#endif // OPAQUEDB_SQL_PARSER_H_

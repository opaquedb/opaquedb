#ifndef OPAQUEDB_SQL_PARSER_H_
#define OPAQUEDB_SQL_PARSER_H_

#include <optional>
#include <string>
#include <string_view>

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
//   statement  := SELECT columns FROM identifier WHERE predicate
//   columns    := identifier (',' identifier)*
//   predicate  := or_term (OR or_term)*
//   or_term    := and_term (AND and_term)*
//   and_term   := identifier comparison
//   comparison := ('=' | '<>' | '<' | '<=' | '>' | '>=') parameter
//               | IN '(' parameter (',' parameter)* ')'
//               | LIKE parameter
//               | BETWEEN parameter AND parameter
// The WHERE value is a bound parameter or an inline literal. A literal is the
// secret value, so the client strips it before sending (see
// PrepareClientQuery).

namespace opaquedb::sql {

absl::StatusOr<SelectStatement> Parse(std::string_view sql);

// The server-facing form of a client's query: a template with no inline
// literal, plus the literal value (if the original had one) for the client to
// encrypt. If the input already uses a bound parameter, the template is
// returned unchanged and literal is nullopt. Run on the client so a literal
// value never crosses the wire.
struct PreparedQuery {
  std::string sql_template;           // parameterized, safe to send
  std::optional<core::Value> literal; // the value to encrypt, if any
  std::string param_name;             // the bound name used for the literal
};

absl::StatusOr<PreparedQuery> PrepareClientQuery(std::string_view sql);

} // namespace opaquedb::sql

#endif // OPAQUEDB_SQL_PARSER_H_

#ifndef OPAQUEDB_SQL_LOGICAL_PLAN_H_
#define OPAQUEDB_SQL_LOGICAL_PLAN_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/sql/ast.h"

// The logical plan is the resolved, executable shape of a statement: the table,
// the columns to project, and the single equality predicate the engine runs. It
// is built from the AST by a visitor. Statements that parse but are not yet
// executable (range, IN, LIKE, BETWEEN, AND, OR, multiple predicates) yield a
// clear UNIMPLEMENTED status here, not a parse error, so the grammar and AST
// are already in place for them.
//
// This plan is still schema-agnostic. Resolving column encodings and choosing a
// backend by capability is the planner's job (the next layer up).

namespace opaquedb::sql {

struct LogicalPlan {
  std::string table;
  std::vector<std::string> projection;
  bool select_all = false; // SELECT *; the planner expands against the schema
  bool count = false;      // SELECT COUNT(*): return the match count, no rows
  std::string match_column;
  CompareOp op = CompareOp::kEq;
  std::string parameter; // the bound parameter name for the (first) match value
  // How many encrypted operands the match carries. 1 is a point query; IN
  // (...) and same-column OR carry one per listed value, matched as a union.
  std::size_t match_operands = 1;
  // Public result window. Unset limit means the default of 1 (single match);
  // offset counts result buckets. The engine resolves these into a bucket count
  // and a window against the configured result_buckets.
  std::optional<std::uint64_t> limit;
  std::optional<std::uint64_t> offset;
};

// Assembles a LogicalPlan field by field and validates it on Build. Used by the
// AST-to-plan conversion so plan construction has one place that enforces the
// invariants (non-empty table, at least one projected column, a bound match).
class LogicalPlanBuilder {
public:
  LogicalPlanBuilder &SetTable(std::string table);
  LogicalPlanBuilder &AddProjection(std::string column);
  LogicalPlanBuilder &SetSelectAll();
  LogicalPlanBuilder &SetCount();
  LogicalPlanBuilder &SetMatch(std::string column, CompareOp op,
                               std::string parameter);
  LogicalPlanBuilder &SetMatchOperands(std::size_t operands);
  LogicalPlanBuilder &SetLimit(std::uint64_t limit);
  LogicalPlanBuilder &SetOffset(std::uint64_t offset);

  absl::StatusOr<LogicalPlan> Build() const;

private:
  LogicalPlan plan_;
  bool match_set_ = false;
};

// Converts a parsed statement into a logical plan, or returns UNIMPLEMENTED for
// a statement whose shape is parsed but not yet executable.
absl::StatusOr<LogicalPlan> BuildLogicalPlan(const SelectStatement &statement);

} // namespace opaquedb::sql

#endif // OPAQUEDB_SQL_LOGICAL_PLAN_H_

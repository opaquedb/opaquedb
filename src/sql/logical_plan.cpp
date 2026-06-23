#include "opaquedb/sql/logical_plan.h"

#include <set>
#include <utility>

#include "absl/strings/str_cat.h"

namespace opaquedb::sql {
namespace {

// Walks the WHERE predicate and fills a plan builder. Only a single equality
// against a bound parameter is executable now; every other node type reports a
// clear UNIMPLEMENTED so the caller knows the statement parsed but cannot yet
// run. This is the planner-side use of the visitor pattern.
class PlanningVisitor : public PredicateVisitor {
public:
  explicit PlanningVisitor(LogicalPlanBuilder &builder) : builder_(builder) {}

  absl::Status Visit(const ComparisonPredicate &node) override {
    if (node.op() != CompareOp::kEq) {
      return absl::UnimplementedError(absl::StrCat(
          "operator ", ToString(node.op()),
          " is parsed but not yet evaluated; only '=' is supported"));
    }
    // A template reaching the plan builder must be parameterized. An inline
    // literal is the secret value; the client strips it before sending, so its
    // presence here means it leaked. Refuse rather than match on it.
    if (node.parameter().is_literal()) {
      return absl::InvalidArgumentError(
          "the match value must be a bound parameter (:name); an inline "
          "literal must be encrypted by the client, not sent in the template");
    }
    builder_.SetMatch(node.column(), node.op(), node.parameter().name);
    return absl::OkStatus();
  }

  absl::Status Visit(const InPredicate &) override {
    return absl::UnimplementedError("IN is parsed but not yet evaluated");
  }
  absl::Status Visit(const LikePredicate &) override {
    return absl::UnimplementedError("LIKE is parsed but not yet evaluated");
  }
  absl::Status Visit(const BetweenPredicate &) override {
    return absl::UnimplementedError("BETWEEN is parsed but not yet evaluated");
  }
  absl::Status Visit(const AndPredicate &) override {
    return absl::UnimplementedError(
        "multiple predicates joined by AND are parsed but not yet evaluated");
  }
  absl::Status Visit(const OrPredicate &) override {
    return absl::UnimplementedError(
        "multiple predicates joined by OR are parsed but not yet evaluated");
  }

private:
  LogicalPlanBuilder &builder_;
};

} // namespace

LogicalPlanBuilder &LogicalPlanBuilder::SetTable(std::string table) {
  plan_.table = std::move(table);
  return *this;
}

LogicalPlanBuilder &LogicalPlanBuilder::AddProjection(std::string column) {
  plan_.projection.push_back(std::move(column));
  return *this;
}

LogicalPlanBuilder &LogicalPlanBuilder::SetSelectAll() {
  plan_.select_all = true;
  return *this;
}

LogicalPlanBuilder &LogicalPlanBuilder::SetMatch(std::string column,
                                                 CompareOp op,
                                                 std::string parameter) {
  plan_.match_column = std::move(column);
  plan_.op = op;
  plan_.parameter = std::move(parameter);
  match_set_ = true;
  return *this;
}

absl::StatusOr<LogicalPlan> LogicalPlanBuilder::Build() const {
  if (plan_.table.empty()) {
    return absl::InvalidArgumentError("logical plan: table is empty");
  }
  if (plan_.projection.empty() && !plan_.select_all) {
    return absl::InvalidArgumentError("logical plan: no columns to project");
  }
  std::set<std::string> seen;
  for (const std::string &column : plan_.projection) {
    if (column.empty()) {
      return absl::InvalidArgumentError(
          "logical plan: a projected column name is empty");
    }
    if (!seen.insert(column).second) {
      return absl::InvalidArgumentError(absl::StrCat(
          "logical plan: column '", column, "' is projected twice"));
    }
  }
  if (!match_set_ || plan_.match_column.empty() || plan_.parameter.empty()) {
    return absl::InvalidArgumentError(
        "logical plan: the WHERE clause has no bound equality match");
  }
  return plan_;
}

absl::StatusOr<LogicalPlan> BuildLogicalPlan(const SelectStatement &statement) {
  LogicalPlanBuilder builder;
  builder.SetTable(statement.table);
  if (statement.select_all) {
    builder.SetSelectAll();
  }
  for (const std::string &column : statement.projection) {
    builder.AddProjection(column);
  }
  if (statement.where == nullptr) {
    return absl::InvalidArgumentError("logical plan: statement has no WHERE");
  }
  PlanningVisitor visitor(builder);
  if (absl::Status s = statement.where->Accept(visitor); !s.ok())
    return s;
  return builder.Build();
}

} // namespace opaquedb::sql

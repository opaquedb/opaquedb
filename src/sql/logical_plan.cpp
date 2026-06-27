#include "opaquedb/sql/logical_plan.h"

#include <set>
#include <utility>

#include "absl/strings/str_cat.h"

namespace opaquedb::sql {
namespace {

// Flattens an OR tree of single-column equalities into the list of bound
// parameter names, the operands of a union match. Anything else (a non-equality
// term, a literal, a second column, a nested AND/IN/LIKE/BETWEEN) means the OR
// is not a simple same-column union and is reported as not yet evaluated.
class OrEqualityCollector : public PredicateVisitor {
public:
  const std::string &column() const { return column_; }
  const std::vector<std::string> &parameters() const { return parameters_; }

  absl::Status Visit(const ComparisonPredicate &node) override {
    if (node.op() != CompareOp::kEq) {
      return absl::UnimplementedError(
          "OR currently supports only '=' terms on one column");
    }
    if (node.parameter().is_literal()) {
      return absl::InvalidArgumentError(
          "OR values must be bound parameters; the client encrypts inline "
          "literals, they are never sent in the template");
    }
    if (column_.empty()) {
      column_ = node.column();
    } else if (column_ != node.column()) {
      return absl::UnimplementedError(
          "OR across different columns needs matching two columns at once and "
          "is not yet evaluated; use OR on a single column or AND");
    }
    parameters_.push_back(node.parameter().name);
    return absl::OkStatus();
  }
  absl::Status Visit(const OrPredicate &node) override {
    if (absl::Status s = node.left().Accept(*this); !s.ok())
      return s;
    return node.right().Accept(*this);
  }
  absl::Status Visit(const InPredicate &) override {
    return absl::UnimplementedError("IN nested inside OR is not yet evaluated");
  }
  absl::Status Visit(const LikePredicate &) override {
    return absl::UnimplementedError("LIKE inside OR is not yet evaluated");
  }
  absl::Status Visit(const BetweenPredicate &) override {
    return absl::UnimplementedError("BETWEEN inside OR is not yet evaluated");
  }
  absl::Status Visit(const AndPredicate &) override {
    return absl::UnimplementedError(
        "AND inside OR is not yet evaluated; OR must be a flat union of "
        "single-column equalities");
  }

private:
  std::string column_;
  std::vector<std::string> parameters_;
};

// Walks the WHERE predicate and fills a plan builder. A single equality, an IN
// list, or an OR of same-column equalities against bound parameters is
// executable; every other node type reports a clear UNIMPLEMENTED so the caller
// knows the statement parsed but cannot yet run. This is the planner-side use
// of the visitor pattern.
class PlanningVisitor : public PredicateVisitor {
public:
  explicit PlanningVisitor(LogicalPlanBuilder &builder) : builder_(builder) {}

  absl::Status Visit(const ComparisonPredicate &node) override {
    // Equality and inequality are evaluable on a searchable column: the matcher
    // builds the same equality indicator and, for '<>', flips it. Ordered
    // comparisons (< <= > >=) still need a range encoding and are not yet
    // evaluated.
    if (node.op() != CompareOp::kEq && node.op() != CompareOp::kNe) {
      return absl::UnimplementedError(absl::StrCat(
          "operator ", ToString(node.op()),
          " is parsed but not yet evaluated; only '=' and '<>' are supported"));
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

  absl::Status Visit(const InPredicate &node) override {
    // col IN (:a, :b, ...): a union of equalities on one column. Every value
    // must be a bound parameter (the client lifts inline literals before
    // sending), and there must be at least one.
    if (node.parameters().empty()) {
      return absl::InvalidArgumentError("IN list is empty");
    }
    for (const Parameter &p : node.parameters()) {
      if (p.is_literal()) {
        return absl::InvalidArgumentError(
            "IN values must be bound parameters; the client encrypts inline "
            "literals, they are never sent in the template");
      }
    }
    builder_.SetMatch(node.column(), CompareOp::kEq,
                      node.parameters().front().name);
    builder_.SetMatchOperands(node.parameters().size());
    return absl::OkStatus();
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
  // col = :a OR col = :b OR ...: a union of equalities. The whole OR tree must
  // be equalities on one column with bound parameters; that is exactly IN, so
  // it routes the same multi-operand union. OR across different columns needs
  // matching two columns at once and is not yet evaluated.
  absl::Status Visit(const OrPredicate &node) override {
    OrEqualityCollector collector;
    if (absl::Status s = node.Accept(collector); !s.ok())
      return s;
    builder_.SetMatch(collector.column(), CompareOp::kEq,
                      collector.parameters().front());
    builder_.SetMatchOperands(collector.parameters().size());
    return absl::OkStatus();
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

LogicalPlanBuilder &LogicalPlanBuilder::SetCount() {
  plan_.count = true;
  return *this;
}

LogicalPlanBuilder &LogicalPlanBuilder::SetMatchOperands(std::size_t operands) {
  plan_.match_operands = operands;
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

LogicalPlanBuilder &LogicalPlanBuilder::SetLimit(std::uint64_t limit) {
  plan_.limit = limit;
  return *this;
}

LogicalPlanBuilder &LogicalPlanBuilder::SetOffset(std::uint64_t offset) {
  plan_.offset = offset;
  return *this;
}

absl::StatusOr<LogicalPlan> LogicalPlanBuilder::Build() const {
  if (plan_.table.empty()) {
    return absl::InvalidArgumentError("logical plan: table is empty");
  }
  if (plan_.projection.empty() && !plan_.select_all && !plan_.count) {
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
  if (statement.count_star) {
    builder.SetCount();
  }
  for (const std::string &column : statement.projection) {
    builder.AddProjection(column);
  }
  if (statement.limit.has_value()) {
    builder.SetLimit(*statement.limit);
  }
  if (statement.offset.has_value()) {
    builder.SetOffset(*statement.offset);
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

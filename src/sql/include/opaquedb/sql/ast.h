#ifndef OPAQUEDB_SQL_AST_H_
#define OPAQUEDB_SQL_AST_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "opaquedb/core/record_codec.h"

// The SQL abstract syntax tree. The grammar supported now is
//   SELECT col (',' col)* FROM table WHERE predicate
// where the WHERE value is either a bound parameter (:name) or an inline
// literal. The value is the secret, so a literal is client-side sugar only: the
// client extracts and encrypts it and rewrites the template to a bound
// parameter before sending. A literal must never reach the server. The SQL
// text, the template, is public.
//
// The AST already has nodes for the operators that are parsed but not yet
// evaluated (IN, LIKE, range comparisons, BETWEEN, AND, OR). They parse into
// these nodes so that implementing them later is filling in evaluation, not
// reparsing. The tree is walked by visitors (the plan builder and a validator),
// so adding a node type extends cleanly.

namespace opaquedb::sql {

// The comparison operators the grammar accepts. Only kEq is evaluable now; the
// rest are parse-only and the plan builder reports them as unimplemented.
enum class CompareOp { kEq, kNe, kLt, kLe, kGt, kGe };

std::string ToString(CompareOp op);

// The matched value in a predicate. Either a bound parameter written ':name',
// whose value arrives encrypted at execution time, or an inline literal the
// client must strip before sending. Exactly one of the two is set.
struct Parameter {
  std::string name;                   // bound parameter name; empty if literal
  std::optional<core::Value> literal; // set when the template used a literal

  bool is_literal() const { return literal.has_value(); }
};

// Forward declarations so the visitor can name every node type.
class ComparisonPredicate;
class InPredicate;
class LikePredicate;
class BetweenPredicate;
class AndPredicate;
class OrPredicate;

// A visitor over predicate nodes. Visit returns a status so a visitor can
// report an unsupported node (the plan builder does this for parse-only
// operators).
class PredicateVisitor {
public:
  virtual ~PredicateVisitor() = default;
  virtual absl::Status Visit(const ComparisonPredicate &node) = 0;
  virtual absl::Status Visit(const InPredicate &node) = 0;
  virtual absl::Status Visit(const LikePredicate &node) = 0;
  virtual absl::Status Visit(const BetweenPredicate &node) = 0;
  virtual absl::Status Visit(const AndPredicate &node) = 0;
  virtual absl::Status Visit(const OrPredicate &node) = 0;
};

class Predicate {
public:
  virtual ~Predicate() = default;
  virtual absl::Status Accept(PredicateVisitor &visitor) const = 0;
};

// column <op> :param
class ComparisonPredicate : public Predicate {
public:
  ComparisonPredicate(std::string column, CompareOp op, Parameter param)
      : column_(std::move(column)), op_(op), param_(std::move(param)) {}

  const std::string &column() const { return column_; }
  CompareOp op() const { return op_; }
  const Parameter &parameter() const { return param_; }

  absl::Status Accept(PredicateVisitor &visitor) const override {
    return visitor.Visit(*this);
  }

private:
  std::string column_;
  CompareOp op_;
  Parameter param_;
};

// column IN (:a, :b, ...)
class InPredicate : public Predicate {
public:
  InPredicate(std::string column, std::vector<Parameter> params)
      : column_(std::move(column)), params_(std::move(params)) {}

  const std::string &column() const { return column_; }
  const std::vector<Parameter> &parameters() const { return params_; }

  absl::Status Accept(PredicateVisitor &visitor) const override {
    return visitor.Visit(*this);
  }

private:
  std::string column_;
  std::vector<Parameter> params_;
};

// column LIKE :param. Whether this is a prefix, suffix, or substring match is
// decided later by the column encoding declared in the schema, not by the
// template, so the node carries only the column and the bound parameter.
class LikePredicate : public Predicate {
public:
  LikePredicate(std::string column, Parameter param)
      : column_(std::move(column)), param_(std::move(param)) {}

  const std::string &column() const { return column_; }
  const Parameter &parameter() const { return param_; }

  absl::Status Accept(PredicateVisitor &visitor) const override {
    return visitor.Visit(*this);
  }

private:
  std::string column_;
  Parameter param_;
};

// column BETWEEN :low AND :high
class BetweenPredicate : public Predicate {
public:
  BetweenPredicate(std::string column, Parameter low, Parameter high)
      : column_(std::move(column)), low_(std::move(low)),
        high_(std::move(high)) {}

  const std::string &column() const { return column_; }
  const Parameter &low() const { return low_; }
  const Parameter &high() const { return high_; }

  absl::Status Accept(PredicateVisitor &visitor) const override {
    return visitor.Visit(*this);
  }

private:
  std::string column_;
  Parameter low_;
  Parameter high_;
};

// left AND right
class AndPredicate : public Predicate {
public:
  AndPredicate(std::unique_ptr<Predicate> left,
               std::unique_ptr<Predicate> right)
      : left_(std::move(left)), right_(std::move(right)) {}

  const Predicate &left() const { return *left_; }
  const Predicate &right() const { return *right_; }

  absl::Status Accept(PredicateVisitor &visitor) const override {
    return visitor.Visit(*this);
  }

private:
  std::unique_ptr<Predicate> left_;
  std::unique_ptr<Predicate> right_;
};

// left OR right
class OrPredicate : public Predicate {
public:
  OrPredicate(std::unique_ptr<Predicate> left, std::unique_ptr<Predicate> right)
      : left_(std::move(left)), right_(std::move(right)) {}

  const Predicate &left() const { return *left_; }
  const Predicate &right() const { return *right_; }

  absl::Status Accept(PredicateVisitor &visitor) const override {
    return visitor.Visit(*this);
  }

private:
  std::unique_ptr<Predicate> left_;
  std::unique_ptr<Predicate> right_;
};

// A parsed SELECT statement. When select_all is true (SELECT *), projection is
// empty and means every column; the planner expands it against the schema.
//
// limit and offset are the optional LIMIT/OFFSET clause. They are public (part
// of the template), not secret. Unset limit means the default of 1, the
// single-match behavior. offset counts result buckets, not rows: see the
// bucketed multi-result design in the reference backend.
struct SelectStatement {
  std::vector<std::string> projection; // columns to return, in order
  bool select_all = false;             // SELECT *
  std::string table;
  std::unique_ptr<Predicate> where;
  std::optional<std::uint64_t> limit;  // LIMIT n; unset means default 1
  std::optional<std::uint64_t> offset; // OFFSET m; unset means 0
};

} // namespace opaquedb::sql

#endif // OPAQUEDB_SQL_AST_H_

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "opaquedb/core/schema.h"
#include "opaquedb/sql/ast.h"
#include "opaquedb/sql/ddl.h"
#include "opaquedb/sql/logical_plan.h"
#include "opaquedb/sql/parser.h"
#include "opaquedb/sql/tokenizer.h"

namespace {

using opaquedb::core::ColumnEncoding;
using opaquedb::core::ColumnType;
using opaquedb::sql::AndPredicate;
using opaquedb::sql::BetweenPredicate;
using opaquedb::sql::BuildLogicalPlan;
using opaquedb::sql::CompareOp;
using opaquedb::sql::ComparisonPredicate;
using opaquedb::sql::InPredicate;
using opaquedb::sql::LikePredicate;
using opaquedb::sql::LogicalPlanBuilder;
using opaquedb::sql::OrPredicate;
using opaquedb::sql::Parse;
using opaquedb::sql::ParseCreateTable;
using opaquedb::sql::Predicate;
using opaquedb::sql::PredicateVisitor;
using opaquedb::sql::SelectStatement;
using opaquedb::sql::Tokenize;
using opaquedb::sql::TokenType;

// ---- Tokenizer -----------------------------------------------------------

TEST(Tokenizer, TokenizesABasicQuery) {
  auto tokens = Tokenize("SELECT a, b FROM t WHERE k = :p");
  ASSERT_TRUE(tokens.ok()) << tokens.status().message();
  std::vector<TokenType> types;
  for (const auto &t : *tokens)
    types.push_back(t.type);
  EXPECT_EQ(types,
            (std::vector<TokenType>{TokenType::kSelect, TokenType::kIdentifier,
                                    TokenType::kComma, TokenType::kIdentifier,
                                    TokenType::kFrom, TokenType::kIdentifier,
                                    TokenType::kWhere, TokenType::kIdentifier,
                                    TokenType::kEq, TokenType::kParameter,
                                    TokenType::kEnd}));
  EXPECT_EQ((*tokens)[9].text, "p"); // parameter name without the colon
}

TEST(Tokenizer, KeywordsAreCaseInsensitiveIdentifiersAreNot) {
  auto tokens = Tokenize("select Col from T where Col = :V");
  ASSERT_TRUE(tokens.ok());
  EXPECT_EQ((*tokens)[0].type, TokenType::kSelect);
  EXPECT_EQ((*tokens)[1].text, "Col"); // identifier case preserved
  EXPECT_EQ((*tokens)[5].text, "Col");
}

TEST(Tokenizer, RecognizesAllOperators) {
  auto tokens = Tokenize("< <= > >= <> != = ( ) ,");
  ASSERT_TRUE(tokens.ok());
  EXPECT_EQ((*tokens)[0].type, TokenType::kLt);
  EXPECT_EQ((*tokens)[1].type, TokenType::kLe);
  EXPECT_EQ((*tokens)[2].type, TokenType::kGt);
  EXPECT_EQ((*tokens)[3].type, TokenType::kGe);
  EXPECT_EQ((*tokens)[4].type, TokenType::kNe);
  EXPECT_EQ((*tokens)[5].type, TokenType::kNe);
  EXPECT_EQ((*tokens)[6].type, TokenType::kEq);
}

TEST(Tokenizer, RejectsBadInput) {
  EXPECT_FALSE(Tokenize("SELECT a FROM t WHERE k = :").ok()); // bare colon
  EXPECT_FALSE(Tokenize("SELECT a FROM t WHERE k = @").ok()); // stray char
  EXPECT_FALSE(Tokenize("SELECT a WHERE x = 'oops").ok());    // unterminated
  EXPECT_FALSE(Tokenize(std::string(70000, 'a')).ok());       // over length
}

// ---- Parser --------------------------------------------------------------

const ComparisonPredicate *AsComparison(const Predicate *p) {
  return dynamic_cast<const ComparisonPredicate *>(p);
}

TEST(Parser, ParsesSupportedSelect) {
  auto stmt = Parse("SELECT v, w FROM users WHERE id = :id");
  ASSERT_TRUE(stmt.ok()) << stmt.status().message();
  EXPECT_EQ(stmt->table, "users");
  EXPECT_EQ(stmt->projection, (std::vector<std::string>{"v", "w"}));
  const ComparisonPredicate *cmp = AsComparison(stmt->where.get());
  ASSERT_NE(cmp, nullptr);
  EXPECT_EQ(cmp->column(), "id");
  EXPECT_EQ(cmp->op(), CompareOp::kEq);
  EXPECT_EQ(cmp->parameter().name, "id");
}

TEST(Parser, ParsesEachComparisonOperator) {
  struct Case {
    std::string sql;
    CompareOp op;
  };
  for (const Case &c :
       std::vector<Case>{{"SELECT a FROM t WHERE k <> :p", CompareOp::kNe},
                         {"SELECT a FROM t WHERE k < :p", CompareOp::kLt},
                         {"SELECT a FROM t WHERE k <= :p", CompareOp::kLe},
                         {"SELECT a FROM t WHERE k > :p", CompareOp::kGt},
                         {"SELECT a FROM t WHERE k >= :p", CompareOp::kGe}}) {
    auto stmt = Parse(c.sql);
    ASSERT_TRUE(stmt.ok()) << c.sql << ": " << stmt.status().message();
    const ComparisonPredicate *cmp = AsComparison(stmt->where.get());
    ASSERT_NE(cmp, nullptr) << c.sql;
    EXPECT_EQ(cmp->op(), c.op) << c.sql;
  }
}

TEST(Parser, ParsesFutureOperatorsIntoNodes) {
  auto in = Parse("SELECT a FROM t WHERE k IN (:a, :b, :c)");
  ASSERT_TRUE(in.ok()) << in.status().message();
  const auto *in_node = dynamic_cast<const InPredicate *>(in->where.get());
  ASSERT_NE(in_node, nullptr);
  EXPECT_EQ(in_node->parameters().size(), 3u);

  auto like = Parse("SELECT a FROM t WHERE name LIKE :pat");
  ASSERT_TRUE(like.ok());
  EXPECT_NE(dynamic_cast<const LikePredicate *>(like->where.get()), nullptr);

  auto between = Parse("SELECT a FROM t WHERE k BETWEEN :lo AND :hi");
  ASSERT_TRUE(between.ok()) << between.status().message();
  const auto *bt = dynamic_cast<const BetweenPredicate *>(between->where.get());
  ASSERT_NE(bt, nullptr);
  EXPECT_EQ(bt->low().name, "lo");
  EXPECT_EQ(bt->high().name, "hi");

  auto conj = Parse("SELECT a FROM t WHERE k = :p AND j = :q");
  ASSERT_TRUE(conj.ok());
  EXPECT_NE(dynamic_cast<const AndPredicate *>(conj->where.get()), nullptr);

  auto disj = Parse("SELECT a FROM t WHERE k = :p OR j = :q");
  ASSERT_TRUE(disj.ok());
  EXPECT_NE(dynamic_cast<const OrPredicate *>(disj->where.get()), nullptr);
}

TEST(Parser, ParsesInlineLiteralValue) {
  // The parser now accepts literals; they are the client's sugar. Both single
  // and double quotes are strings.
  for (const char *sql : {"SELECT a FROM t WHERE k = 'London'",
                          "SELECT a FROM t WHERE k = \"London\""}) {
    auto stmt = Parse(sql);
    ASSERT_TRUE(stmt.ok()) << sql;
    const auto *cmp =
        dynamic_cast<const ComparisonPredicate *>(stmt->where.get());
    ASSERT_NE(cmp, nullptr);
    ASSERT_TRUE(cmp->parameter().is_literal());
    EXPECT_EQ(std::get<std::string>(*cmp->parameter().literal), "London");
  }
  auto num = Parse("SELECT a FROM t WHERE k = 42");
  ASSERT_TRUE(num.ok());
  const auto *cmp = dynamic_cast<const ComparisonPredicate *>(num->where.get());
  ASSERT_NE(cmp, nullptr);
  ASSERT_TRUE(cmp->parameter().is_literal());
  EXPECT_EQ(std::get<std::int64_t>(*cmp->parameter().literal), 42);
}

TEST(LogicalPlan, RejectsLiteralBecauseItMustBeStrippedClientSide) {
  // A literal must never reach the server; the plan builder refuses it.
  auto stmt = Parse("SELECT a FROM t WHERE k = 'London'");
  ASSERT_TRUE(stmt.ok());
  auto plan = BuildLogicalPlan(*stmt);
  EXPECT_FALSE(plan.ok());
  EXPECT_EQ(plan.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(Parser, PrepareClientQueryStripsLiteral) {
  using opaquedb::sql::PrepareClientQuery;
  auto p = PrepareClientQuery("SELECT temperature FROM weather WHERE city = "
                              "\"London\"");
  ASSERT_TRUE(p.ok());
  ASSERT_EQ(p->literals.size(), 1u);
  EXPECT_EQ(std::get<std::string>(p->literals[0]), "London");
  // The rewritten template carries a bound parameter, no literal, and parses
  // and plans cleanly server-side.
  EXPECT_EQ(p->sql_template, "SELECT temperature FROM weather WHERE city = :v");
  auto stmt = Parse(p->sql_template);
  ASSERT_TRUE(stmt.ok());
  EXPECT_TRUE(BuildLogicalPlan(*stmt).ok());

  // An already-parameterized query passes through unchanged.
  auto q = PrepareClientQuery("SELECT a FROM t WHERE k = :id");
  ASSERT_TRUE(q.ok());
  EXPECT_TRUE(q->literals.empty());
  EXPECT_EQ(q->sql_template, "SELECT a FROM t WHERE k = :id");
}

TEST(Parser, PrepareClientQueryLiftsInListAndCountStar) {
  using opaquedb::sql::Parse;
  using opaquedb::sql::PrepareClientQuery;
  // An IN list lifts every literal in order, rewriting to :v0, :v1, ... so the
  // server template parses back to the same shape and plans.
  auto p =
      PrepareClientQuery("SELECT city FROM weather WHERE id IN (17, 42, 99)");
  ASSERT_TRUE(p.ok()) << p.status().message();
  ASSERT_EQ(p->literals.size(), 3u);
  EXPECT_EQ(std::get<std::int64_t>(p->literals[0]), 17);
  EXPECT_EQ(std::get<std::int64_t>(p->literals[2]), 99);
  EXPECT_EQ(p->sql_template,
            "SELECT city FROM weather WHERE id IN (:v0, :v1, :v2)");
  auto stmt = Parse(p->sql_template);
  ASSERT_TRUE(stmt.ok());
  EXPECT_TRUE(BuildLogicalPlan(*stmt).ok());

  // COUNT(*) is flagged so the client reads the match count.
  auto c = PrepareClientQuery("SELECT COUNT(*) FROM weather WHERE city = "
                              "\"London\"");
  ASSERT_TRUE(c.ok()) << c.status().message();
  EXPECT_TRUE(c->count);
  ASSERT_EQ(c->literals.size(), 1u);
  EXPECT_EQ(c->sql_template, "SELECT COUNT(*) FROM weather WHERE city = :v");
}

TEST(Parser, ParsesSelectStar) {
  auto stmt = Parse("SELECT * FROM weather WHERE city = :c");
  ASSERT_TRUE(stmt.ok()) << stmt.status().message();
  EXPECT_TRUE(stmt->select_all);
  EXPECT_TRUE(stmt->projection.empty());
  EXPECT_EQ(stmt->table, "weather");
}

TEST(Parser, ParsesLimitAndOffset) {
  auto both = Parse("SELECT a FROM t WHERE k = :p LIMIT 5 OFFSET 10");
  ASSERT_TRUE(both.ok()) << both.status().message();
  ASSERT_TRUE(both->limit.has_value());
  EXPECT_EQ(*both->limit, 5u);
  ASSERT_TRUE(both->offset.has_value());
  EXPECT_EQ(*both->offset, 10u);

  // Both clauses are optional and independent.
  auto none = Parse("SELECT a FROM t WHERE k = :p");
  ASSERT_TRUE(none.ok());
  EXPECT_FALSE(none->limit.has_value());
  EXPECT_FALSE(none->offset.has_value());

  auto limit_only = Parse("SELECT a FROM t WHERE k = :p LIMIT 3");
  ASSERT_TRUE(limit_only.ok());
  EXPECT_EQ(*limit_only->limit, 3u);
  EXPECT_FALSE(limit_only->offset.has_value());

  auto offset_only = Parse("SELECT a FROM t WHERE k = :p OFFSET 7");
  ASSERT_TRUE(offset_only.ok());
  EXPECT_FALSE(offset_only->limit.has_value());
  EXPECT_EQ(*offset_only->offset, 7u);

  // LIMIT/OFFSET is case-insensitive and survives a trailing semicolon.
  auto mixed = Parse("select a from t where k = :p limit 2 offset 4;");
  ASSERT_TRUE(mixed.ok()) << mixed.status().message();
  EXPECT_EQ(*mixed->limit, 2u);
  EXPECT_EQ(*mixed->offset, 4u);
}

TEST(Parser, RejectsBadLimitOffset) {
  EXPECT_FALSE(
      Parse("SELECT a FROM t WHERE k = :p LIMIT 0").ok()); // must be >=1
  EXPECT_FALSE(
      Parse("SELECT a FROM t WHERE k = :p LIMIT :n").ok()); // not bound
  EXPECT_FALSE(
      Parse("SELECT a FROM t WHERE k = :p LIMIT").ok()); // missing count
  EXPECT_FALSE(Parse("SELECT a FROM t WHERE k = :p OFFSET 2 LIMIT 1")
                   .ok()); // wrong order
}

TEST(Parser, PrepareClientQueryKeepsLimitAndStripsLiteral) {
  using opaquedb::sql::PrepareClientQuery;
  // The LIMIT/OFFSET numbers must not be mistaken for the secret match value.
  auto p = PrepareClientQuery(
      "SELECT t FROM weather WHERE city = \"London\" LIMIT 4 OFFSET 8");
  ASSERT_TRUE(p.ok()) << p.status().message();
  ASSERT_EQ(p->literals.size(), 1u);
  EXPECT_EQ(std::get<std::string>(p->literals[0]), "London");
  EXPECT_EQ(p->sql_template,
            "SELECT t FROM weather WHERE city = :v LIMIT 4 OFFSET 8");
  ASSERT_TRUE(p->limit.has_value());
  EXPECT_EQ(*p->limit, 4u);
  ASSERT_TRUE(p->offset.has_value());
  EXPECT_EQ(*p->offset, 8u);
}

TEST(Parser, AcceptsTrailingSemicolon) {
  auto a = Parse("SELECT v FROM t WHERE k = :p;");
  ASSERT_TRUE(a.ok()) << a.status().message();
  EXPECT_EQ(a->projection, (std::vector<std::string>{"v"}));
  auto b = Parse("SELECT * FROM t WHERE k = :p ;  ");
  ASSERT_TRUE(b.ok()) << b.status().message();
  EXPECT_TRUE(b->select_all);
  // A literal query with a trailing semicolon still strips to a clean template.
  auto p = opaquedb::sql::PrepareClientQuery(
      "SELECT v FROM t WHERE city = \"London\";");
  ASSERT_TRUE(p.ok());
  EXPECT_EQ(p->sql_template, "SELECT v FROM t WHERE city = :v");
  ASSERT_EQ(p->literals.size(), 1u);
}

TEST(Parser, RejectsMalformedStatements) {
  EXPECT_FALSE(Parse("").ok());
  EXPECT_FALSE(Parse("SELECT FROM t WHERE k = :p").ok());         // no columns
  EXPECT_FALSE(Parse("SELECT a t WHERE k = :p").ok());            // no FROM
  EXPECT_FALSE(Parse("SELECT a FROM t k = :p").ok());             // no WHERE
  EXPECT_FALSE(Parse("SELECT a FROM t WHERE k :p").ok());         // no operator
  EXPECT_FALSE(Parse("SELECT a FROM t WHERE k = :p extra").ok()); // trailing
  EXPECT_FALSE(Parse("SELECT a FROM t WHERE k IN (:a,)").ok());   // dangling
  EXPECT_FALSE(Parse("SELECT a FROM t WHERE k IN (:a").ok());     // unclosed
  EXPECT_FALSE(Parse("SELECT a FROM t WHERE k BETWEEN :lo :hi").ok()); // no AND
}

// ---- Visitor -------------------------------------------------------------

// A visitor that counts the predicate nodes it reaches, to show the AST is
// walked through the visitor interface.
class CountingVisitor : public PredicateVisitor {
public:
  absl::Status Visit(const ComparisonPredicate &) override {
    ++comparisons;
    return absl::OkStatus();
  }
  absl::Status Visit(const InPredicate &) override { return absl::OkStatus(); }
  absl::Status Visit(const LikePredicate &) override {
    return absl::OkStatus();
  }
  absl::Status Visit(const BetweenPredicate &) override {
    return absl::OkStatus();
  }
  absl::Status Visit(const AndPredicate &node) override {
    ++ands;
    if (auto s = node.left().Accept(*this); !s.ok())
      return s;
    return node.right().Accept(*this);
  }
  absl::Status Visit(const OrPredicate &node) override {
    if (auto s = node.left().Accept(*this); !s.ok())
      return s;
    return node.right().Accept(*this);
  }
  int comparisons = 0;
  int ands = 0;
};

TEST(Visitor, WalksTheTree) {
  auto stmt = Parse("SELECT a FROM t WHERE k = :p AND j = :q AND i = :r");
  ASSERT_TRUE(stmt.ok());
  CountingVisitor visitor;
  ASSERT_TRUE(stmt->where->Accept(visitor).ok());
  EXPECT_EQ(visitor.comparisons, 3);
  EXPECT_EQ(visitor.ands, 2);
}

// ---- Logical plan --------------------------------------------------------

// ---- DDL -----------------------------------------------------------------

TEST(Ddl, ParsesKeyIndexAndPayloadColumns) {
  auto schema =
      ParseCreateTable("CREATE TABLE users (id INT KEY, username TEXT INDEX, "
                       "password TEXT INDEX, email TEXT)");
  ASSERT_TRUE(schema.ok()) << schema.status().message();
  ASSERT_EQ(schema->columns().size(), 4u);
  EXPECT_EQ(schema->columns()[0].encoding, ColumnEncoding::kEq);
  EXPECT_EQ(schema->columns()[1].encoding, ColumnEncoding::kIndex);
  EXPECT_EQ(schema->columns()[2].encoding, ColumnEncoding::kIndex);
  EXPECT_EQ(schema->columns()[3].encoding, ColumnEncoding::kRaw);
  // The primary key plus two indexes are searchable; the raw column is not.
  EXPECT_EQ(schema->SearchableCount(), 3u);
  EXPECT_EQ(schema->SearchableRank(0), 0u);
  EXPECT_EQ(schema->SearchableRank(2), 2u);
  EXPECT_FALSE(schema->SearchableRank(3).has_value());
}

TEST(Ddl, StillRequiresExactlyOneKey) {
  EXPECT_FALSE(ParseCreateTable("CREATE TABLE t (a TEXT INDEX, b TEXT)").ok());
  EXPECT_FALSE(ParseCreateTable("CREATE TABLE t (a INT KEY, b INT KEY)").ok());
}

TEST(Ddl, RejectsRealIndexColumn) {
  EXPECT_FALSE(
      ParseCreateTable("CREATE TABLE t (a INT KEY, b REAL INDEX)").ok());
}

TEST(Ddl, ParsesAJsonPayloadColumn) {
  auto schema = ParseCreateTable("CREATE TABLE docs (id INT KEY, body JSON)");
  ASSERT_TRUE(schema.ok()) << schema.status().message();
  ASSERT_EQ(schema->columns().size(), 2u);
  EXPECT_EQ(schema->columns()[1].type, ColumnType::kJson);
  EXPECT_EQ(schema->columns()[1].encoding, ColumnEncoding::kRaw);
}

TEST(Ddl, RejectsAJsonMatchKey) {
  EXPECT_FALSE(ParseCreateTable("CREATE TABLE t (k JSON KEY)").ok());
  EXPECT_FALSE(
      ParseCreateTable("CREATE TABLE t (a INT KEY, b JSON INDEX)").ok());
}

TEST(LogicalPlan, BuildsForSupportedEquality) {
  auto stmt = Parse("SELECT v, w FROM t WHERE k = :key");
  ASSERT_TRUE(stmt.ok());
  auto plan = BuildLogicalPlan(*stmt);
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_EQ(plan->table, "t");
  EXPECT_EQ(plan->projection, (std::vector<std::string>{"v", "w"}));
  EXPECT_EQ(plan->match_column, "k");
  EXPECT_EQ(plan->op, CompareOp::kEq);
  EXPECT_EQ(plan->parameter, "key");
}

TEST(LogicalPlan, RejectsUnimplementedOperators) {
  const std::vector<std::string> cases = {
      "SELECT a FROM t WHERE k < :p",
      "SELECT a FROM t WHERE name LIKE :p",
      "SELECT a FROM t WHERE k BETWEEN :lo AND :hi",
      "SELECT a FROM t WHERE k = :p AND j = :q",
      "SELECT a FROM t WHERE k = :p OR j = :q", // OR across different columns
  };
  for (const std::string &sql : cases) {
    auto stmt = Parse(sql);
    ASSERT_TRUE(stmt.ok()) << sql << ": " << stmt.status().message();
    auto plan = BuildLogicalPlan(*stmt);
    EXPECT_FALSE(plan.ok()) << sql;
    EXPECT_EQ(plan.status().code(), absl::StatusCode::kUnimplemented) << sql;
  }
}

TEST(LogicalPlan, AcceptsInAndSameColumnOrAsAUnion) {
  // IN lists and a same-column OR plan as a multi-operand union on one column.
  auto in_stmt = Parse("SELECT a FROM t WHERE k IN (:a, :b, :c)");
  ASSERT_TRUE(in_stmt.ok());
  auto in_plan = BuildLogicalPlan(*in_stmt);
  ASSERT_TRUE(in_plan.ok()) << in_plan.status().message();
  EXPECT_EQ(in_plan->match_column, "k");
  EXPECT_EQ(in_plan->op, opaquedb::sql::CompareOp::kEq);
  EXPECT_EQ(in_plan->match_operands, 3u);

  auto or_stmt = Parse("SELECT a FROM t WHERE k = :a OR k = :b");
  ASSERT_TRUE(or_stmt.ok());
  auto or_plan = BuildLogicalPlan(*or_stmt);
  ASSERT_TRUE(or_plan.ok()) << or_plan.status().message();
  EXPECT_EQ(or_plan->match_column, "k");
  EXPECT_EQ(or_plan->match_operands, 2u);

  // An inline literal inside IN must still be refused at the plan builder (the
  // client lifts it before sending).
  auto literal_in = Parse("SELECT a FROM t WHERE k IN (1, 2)");
  ASSERT_TRUE(literal_in.ok());
  EXPECT_FALSE(BuildLogicalPlan(*literal_in).ok());
}

TEST(LogicalPlan, AcceptsCountStar) {
  auto stmt = Parse("SELECT COUNT(*) FROM t WHERE k = :p");
  ASSERT_TRUE(stmt.ok()) << stmt.status().message();
  EXPECT_TRUE(stmt->count_star);
  auto plan = BuildLogicalPlan(*stmt);
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_TRUE(plan->count);
  EXPECT_TRUE(plan->projection.empty());
}

TEST(LogicalPlan, RejectsDuplicateProjection) {
  auto stmt = Parse("SELECT a, a FROM t WHERE k = :p");
  ASSERT_TRUE(stmt.ok());
  EXPECT_FALSE(BuildLogicalPlan(*stmt).ok());
}

TEST(LogicalPlan, BuilderValidatesRequiredFields) {
  LogicalPlanBuilder empty;
  EXPECT_FALSE(empty.Build().ok()); // no table, no projection, no match

  LogicalPlanBuilder no_match;
  no_match.SetTable("t").AddProjection("a");
  EXPECT_FALSE(no_match.Build().ok());

  LogicalPlanBuilder full;
  full.SetTable("t").AddProjection("a").SetMatch("k", CompareOp::kEq, "p");
  EXPECT_TRUE(full.Build().ok());
}

TEST(Parser, AcceptsSelectWithNoWhereAsAScan) {
  auto stmt = Parse("SELECT * FROM weather");
  ASSERT_TRUE(stmt.ok()) << stmt.status().message();
  EXPECT_EQ(stmt->table, "weather");
  EXPECT_TRUE(stmt->select_all);
  EXPECT_EQ(stmt->where, nullptr) << "no WHERE means a full scan";

  auto cols = Parse("SELECT city, conditions FROM weather");
  ASSERT_TRUE(cols.ok()) << cols.status().message();
  EXPECT_EQ(cols->projection, (std::vector<std::string>{"city", "conditions"}));
  EXPECT_EQ(cols->where, nullptr);

  // A no-WHERE COUNT(*) parses too: it is the table's row count.
  auto cnt = Parse("SELECT COUNT(*) FROM weather");
  ASSERT_TRUE(cnt.ok()) << cnt.status().message();
  EXPECT_TRUE(cnt->count_star);
  EXPECT_EQ(cnt->where, nullptr);
}

TEST(Parser, ParsesDistinctOrderByAndAliases) {
  auto stmt = Parse("SELECT DISTINCT city AS town, temperature FROM weather "
                    "ORDER BY temperature DESC, city LIMIT 5 OFFSET 2");
  ASSERT_TRUE(stmt.ok()) << stmt.status().message();
  EXPECT_TRUE(stmt->distinct);
  EXPECT_EQ(stmt->projection,
            (std::vector<std::string>{"city", "temperature"}));
  EXPECT_EQ(stmt->projection_aliases,
            (std::vector<std::string>{"town", "temperature"}));
  ASSERT_EQ(stmt->order_by.size(), 2u);
  EXPECT_EQ(stmt->order_by[0].column, "temperature");
  EXPECT_TRUE(stmt->order_by[0].descending);
  EXPECT_EQ(stmt->order_by[1].column, "city");
  EXPECT_FALSE(stmt->order_by[1].descending); // ASC default
  EXPECT_EQ(stmt->limit, 5u);
  EXPECT_EQ(stmt->offset, 2u);
}

TEST(LogicalPlan, AcceptsInequality) {
  for (const char *sql :
       {"SELECT a FROM t WHERE k <> :p", "SELECT a FROM t WHERE k != :p"}) {
    auto stmt = Parse(sql);
    ASSERT_TRUE(stmt.ok()) << sql << ": " << stmt.status().message();
    auto plan = BuildLogicalPlan(*stmt);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().message();
    EXPECT_EQ(plan->op, CompareOp::kNe) << sql;
    EXPECT_EQ(plan->match_column, "k") << sql;
    EXPECT_EQ(plan->match_operands, 1u) << sql;
  }
}

} // namespace

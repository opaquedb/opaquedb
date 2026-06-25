#include "opaquedb/planner/planner.h"

#include <gtest/gtest.h>

#include <memory>

#include "opaquedb/backend/capabilities.h"
#include "opaquedb/backend/registry.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/sql/logical_plan.h"
#include "opaquedb/sql/parser.h"

namespace {

using opaquedb::backend::BackendCapabilities;
using opaquedb::backend::BackendRegistry;
using opaquedb::backend::Op;
using opaquedb::backend::Privacy;
using opaquedb::core::Column;
using opaquedb::core::ColumnEncoding;
using opaquedb::core::Schema;
using opaquedb::planner::Planner;
using opaquedb::planner::RouteOperator;
using opaquedb::sql::BuildLogicalPlan;
using opaquedb::sql::CompareOp;
using opaquedb::sql::Parse;

// Seeds a registry with one equality backend, so the planner has something to
// select without pulling in the SEAL-backed reference backend. The registry is
// non-movable, so it is populated in place rather than returned.
void SeedEqBackend(BackendRegistry &registry) {
  BackendCapabilities caps;
  caps.name = "fake_eq";
  caps.operators = {Op::kEq};
  caps.privacy_modes = {Privacy::kQueryPrivate};
  registry.register_backend(
      "fake_eq",
      [](const opaquedb::crypto::CryptoContext &) { return nullptr; }, caps);
}

Schema MakeSchema() {
  return Schema("t", {Column{"k", ColumnEncoding::kEq},
                      Column{"v", ColumnEncoding::kRaw}});
}

opaquedb::sql::LogicalPlan PlanFor(const std::string &sql) {
  auto stmt = Parse(sql);
  EXPECT_TRUE(stmt.ok()) << stmt.status().message();
  auto logical = BuildLogicalPlan(*stmt);
  EXPECT_TRUE(logical.ok()) << logical.status().message();
  return *logical;
}

TEST(Planner, RoutesEqualityToABackend) {
  BackendRegistry registry;
  SeedEqBackend(registry);
  Planner planner(registry);
  auto plan =
      planner.Plan(MakeSchema(), PlanFor("SELECT v FROM t WHERE k = :p"));
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  EXPECT_EQ(plan->backend_name, "fake_eq");
  EXPECT_EQ(plan->op, Op::kEq);
  EXPECT_EQ(plan->match_column, "k");
  EXPECT_EQ(plan->match_column_index, 0u);
  EXPECT_EQ(plan->projection, (std::vector<std::string>{"v"}));
  EXPECT_EQ(plan->projection_indices, (std::vector<std::size_t>{1}));
}

TEST(Planner, RejectsUnknownColumns) {
  BackendRegistry registry;
  SeedEqBackend(registry);
  Planner planner(registry);
  EXPECT_FALSE(
      planner.Plan(MakeSchema(), PlanFor("SELECT v FROM t WHERE q = :p")).ok());
  EXPECT_FALSE(
      planner.Plan(MakeSchema(), PlanFor("SELECT z FROM t WHERE k = :p")).ok());
}

TEST(Planner, RejectsWrongTable) {
  BackendRegistry registry;
  SeedEqBackend(registry);
  Planner planner(registry);
  EXPECT_FALSE(
      planner.Plan(MakeSchema(), PlanFor("SELECT v FROM other WHERE k = :p"))
          .ok());
}

TEST(Planner, RejectsMatchOnRawColumn) {
  BackendRegistry registry;
  SeedEqBackend(registry);
  Planner planner(registry);
  // v is RAW: it cannot be matched.
  EXPECT_FALSE(
      planner.Plan(MakeSchema(), PlanFor("SELECT k FROM t WHERE v = :p")).ok());
}

TEST(Planner, HonorsBackendHint) {
  BackendRegistry registry;
  SeedEqBackend(registry);
  Planner planner(registry);
  auto good = planner.Plan(MakeSchema(),
                           PlanFor("SELECT v FROM t WHERE k = :p"), "fake_eq");
  EXPECT_TRUE(good.ok());
  auto bad = planner.Plan(MakeSchema(), PlanFor("SELECT v FROM t WHERE k = :p"),
                          "nope");
  EXPECT_FALSE(bad.ok());
}

TEST(Planner, NoBackendForOperator) {
  BackendRegistry empty; // nothing registered
  Planner planner(empty);
  EXPECT_FALSE(
      planner.Plan(MakeSchema(), PlanFor("SELECT v FROM t WHERE k = :p")).ok());
}

TEST(RouteOperator, EncodingRules) {
  EXPECT_TRUE(RouteOperator(ColumnEncoding::kEq, CompareOp::kEq).ok());
  EXPECT_TRUE(RouteOperator(ColumnEncoding::kEq, CompareOp::kNe).ok());
  EXPECT_FALSE(RouteOperator(ColumnEncoding::kEq, CompareOp::kLt).ok());
  EXPECT_FALSE(RouteOperator(ColumnEncoding::kRaw, CompareOp::kEq).ok());
  EXPECT_EQ(
      RouteOperator(ColumnEncoding::kPrefix, CompareOp::kEq).status().code(),
      absl::StatusCode::kUnimplemented);
}

} // namespace

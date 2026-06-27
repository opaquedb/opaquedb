#include "opaquedb/core/schema.h"

#include <gtest/gtest.h>

#include <vector>

// The schema is the single source of truth for the table layout, read by the
// planner, storage, and the backends, and reconstructed from an untrusted epoch
// manifest. Schema::Validate is the gate that decides whether a layout is even
// usable, so every rejection branch matters: a layout that should be refused
// but is not would let a malformed table reach the matcher. These tests drive
// each rejection and each structural accessor on its own.

namespace {

using opaquedb::core::Column;
using opaquedb::core::ColumnEncoding;
using opaquedb::core::ColumnType;
using opaquedb::core::IsSearchable;
using opaquedb::core::ParseColumnEncoding;
using opaquedb::core::ParseColumnType;
using opaquedb::core::Schema;
using opaquedb::core::ToString;

// A well-formed table: one INT primary key, one TEXT secondary index, one raw
// payload column. Tests below mutate copies of this to trip each branch.
Schema GoodSchema() {
  return Schema("weather", {{"id", ColumnEncoding::kEq, ColumnType::kInt},
                            {"city", ColumnEncoding::kIndex, ColumnType::kText},
                            {"note", ColumnEncoding::kRaw, ColumnType::kText}});
}

TEST(Schema, ValidatesAWellFormedTable) {
  EXPECT_TRUE(GoodSchema().Validate().ok());
}

TEST(Schema, RejectsAnEmptyTableName) {
  Schema s("", {{"id", ColumnEncoding::kEq, ColumnType::kInt}});
  EXPECT_FALSE(s.Validate().ok());
}

TEST(Schema, RejectsNoColumns) {
  Schema s("t", {});
  EXPECT_FALSE(s.Validate().ok());
}

TEST(Schema, RejectsAnEmptyColumnName) {
  Schema s("t", {{"", ColumnEncoding::kEq, ColumnType::kInt}});
  EXPECT_FALSE(s.Validate().ok());
}

TEST(Schema, RejectsDuplicateColumnNames) {
  Schema s("t", {{"id", ColumnEncoding::kEq, ColumnType::kInt},
                 {"id", ColumnEncoding::kRaw, ColumnType::kText}});
  EXPECT_FALSE(s.Validate().ok());
}

TEST(Schema, RejectsZeroMatchKeys) {
  // No kEq column at all: a table needs exactly one primary key.
  Schema s("t", {{"a", ColumnEncoding::kRaw, ColumnType::kInt},
                 {"b", ColumnEncoding::kIndex, ColumnType::kText}});
  EXPECT_FALSE(s.Validate().ok());
}

TEST(Schema, RejectsTwoMatchKeys) {
  Schema s("t", {{"a", ColumnEncoding::kEq, ColumnType::kInt},
                 {"b", ColumnEncoding::kEq, ColumnType::kInt}});
  EXPECT_FALSE(s.Validate().ok());
}

TEST(Schema, RejectsARealPrimaryKey) {
  Schema s("t", {{"k", ColumnEncoding::kEq, ColumnType::kReal}});
  EXPECT_FALSE(s.Validate().ok()) << "equality on REAL is not well defined";
}

TEST(Schema, RejectsARealSecondaryIndex) {
  Schema s("t", {{"id", ColumnEncoding::kEq, ColumnType::kInt},
                 {"x", ColumnEncoding::kIndex, ColumnType::kReal}});
  EXPECT_FALSE(s.Validate().ok());
}

TEST(Schema, AllowsARealRawPayloadColumn) {
  // REAL is only forbidden for searchable columns; as raw payload it is fine.
  Schema s("t", {{"id", ColumnEncoding::kEq, ColumnType::kInt},
                 {"lat", ColumnEncoding::kRaw, ColumnType::kReal}});
  EXPECT_TRUE(s.Validate().ok());
}

TEST(Schema, RejectsAJsonMatchKey) {
  // JSON is payload only; it must never be a match key (primary or index).
  Schema key("t", {{"k", ColumnEncoding::kEq, ColumnType::kJson}});
  EXPECT_FALSE(key.Validate().ok());
  Schema index("t", {{"id", ColumnEncoding::kEq, ColumnType::kInt},
                     {"doc", ColumnEncoding::kIndex, ColumnType::kJson}});
  EXPECT_FALSE(index.Validate().ok());
}

TEST(Schema, AllowsAJsonRawPayloadColumn) {
  Schema s("t", {{"id", ColumnEncoding::kEq, ColumnType::kInt},
                 {"doc", ColumnEncoding::kRaw, ColumnType::kJson}});
  EXPECT_TRUE(s.Validate().ok());
}

TEST(Schema, IsSearchableCoversKeyAndIndexOnly) {
  EXPECT_TRUE(IsSearchable(ColumnEncoding::kEq));
  EXPECT_TRUE(IsSearchable(ColumnEncoding::kIndex));
  EXPECT_FALSE(IsSearchable(ColumnEncoding::kRaw));
  EXPECT_FALSE(IsSearchable(ColumnEncoding::kPrefix));
  EXPECT_FALSE(IsSearchable(ColumnEncoding::kRange));
}

TEST(Schema, IndexOfFindsAndMissesColumns) {
  Schema s = GoodSchema();
  EXPECT_EQ(s.IndexOf("id"), std::optional<std::size_t>(0));
  EXPECT_EQ(s.IndexOf("note"), std::optional<std::size_t>(2));
  EXPECT_EQ(s.IndexOf("absent"), std::nullopt);
}

TEST(Schema, SearchableCountAndRankFollowSchemaOrder) {
  Schema s = GoodSchema(); // searchable: id (rank 0), city (rank 1)
  EXPECT_EQ(s.SearchableCount(), 2u);
  EXPECT_EQ(s.SearchableRank(0), std::optional<std::size_t>(0)); // id
  EXPECT_EQ(s.SearchableRank(1), std::optional<std::size_t>(1)); // city
  EXPECT_EQ(s.SearchableRank(2), std::nullopt);  // note is raw, not searchable
  EXPECT_EQ(s.SearchableRank(99), std::nullopt); // out of range
}

TEST(Schema, IsMatchableTrueExceptForRawAndMissing) {
  Schema s = GoodSchema();
  EXPECT_TRUE(s.IsMatchable("id"));
  EXPECT_TRUE(s.IsMatchable("city"));
  EXPECT_FALSE(s.IsMatchable("note"));   // raw
  EXPECT_FALSE(s.IsMatchable("absent")); // not a column
}

TEST(Schema, EncodingStringsRoundTrip) {
  for (ColumnEncoding e :
       {ColumnEncoding::kEq, ColumnEncoding::kIndex, ColumnEncoding::kPrefix,
        ColumnEncoding::kRange, ColumnEncoding::kRaw}) {
    auto parsed = ParseColumnEncoding(ToString(e));
    ASSERT_TRUE(parsed.ok()) << ToString(e);
    EXPECT_EQ(*parsed, e);
  }
  EXPECT_FALSE(ParseColumnEncoding("bogus").ok());
  EXPECT_FALSE(ParseColumnEncoding("").ok());
}

TEST(Schema, TypeStringsRoundTrip) {
  for (ColumnType t : {ColumnType::kInt, ColumnType::kReal, ColumnType::kText,
                       ColumnType::kJson}) {
    auto parsed = ParseColumnType(ToString(t));
    ASSERT_TRUE(parsed.ok()) << ToString(t);
    EXPECT_EQ(*parsed, t);
  }
  EXPECT_FALSE(ParseColumnType("bogus").ok());
  EXPECT_FALSE(ParseColumnType("INT").ok()) << "parse is case sensitive";
}

} // namespace

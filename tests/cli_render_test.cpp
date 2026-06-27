#include "../src/cli/util.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "opaquedb/core/record_codec.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/sql/parser.h"

// Unit tests for the client-side result presentation: aligned table rendering
// and the DISTINCT / ORDER BY / projection / LIMIT-OFFSET pipeline that runs
// over decoded rows. These are the parts the server never sees.

namespace {

using opaquedb::cli::BuildResultTable;
using opaquedb::cli::RenderTable;
using opaquedb::core::Column;
using opaquedb::core::ColumnEncoding;
using opaquedb::core::ColumnType;
using opaquedb::core::Schema;
using opaquedb::core::Value;

constexpr std::uint32_t kRecordBytes = 32;

// id INT KEY, city TEXT, temp INT. Payload columns are city then temp.
Schema WeatherSchema() {
  return Schema("weather",
                {Column{"id", ColumnEncoding::kEq, ColumnType::kInt},
                 Column{"city", ColumnEncoding::kRaw, ColumnType::kText},
                 Column{"temp", ColumnEncoding::kRaw, ColumnType::kInt}});
}

std::vector<std::uint8_t> Row(const std::string &city, std::int64_t temp) {
  auto rec = opaquedb::core::EncodeRecord(
      WeatherSchema(), {Value{city}, Value{temp}}, kRecordBytes);
  EXPECT_TRUE(rec.ok()) << rec.status().message();
  return *rec;
}

opaquedb::sql::SelectStatement Parse(const std::string &sql) {
  auto stmt = opaquedb::sql::Parse(sql);
  EXPECT_TRUE(stmt.ok()) << sql << ": " << stmt.status().message();
  return *std::move(stmt);
}

// Extracts the city column (first cell) of each rendered row, in order.
std::vector<std::string> FirstCells(const opaquedb::cli::TableData &t) {
  std::vector<std::string> out;
  for (const auto &row : t.rows)
    out.push_back(row.empty() ? "" : row.front());
  return out;
}

TEST(RenderTable, AlignsColumnsWithHeaderAndSeparator) {
  std::string out =
      RenderTable({"city", "temp"}, {{"London", "15"}, {"Amsterdam", "9"}});
  // The header pads to the widest cell ("Amsterdam"), and a dashed separator
  // line sits under it.
  EXPECT_NE(out.find("city"), std::string::npos);
  EXPECT_NE(out.find("Amsterdam"), std::string::npos);
  EXPECT_NE(out.find("-+-"), std::string::npos) << "column separator in rule";
  EXPECT_NE(out.find(" London    "), std::string::npos)
      << "short cell padded to the column width";
}

TEST(BuildResultTable, ProjectionAndAliasesSetHeaders) {
  auto stmt = Parse("SELECT city AS town, temp FROM weather");
  auto t = BuildResultTable(WeatherSchema(), {Row("London", 15)}, stmt);
  ASSERT_TRUE(t.ok()) << t.status().message();
  EXPECT_EQ(t->headers, (std::vector<std::string>{"town", "temp"}));
  ASSERT_EQ(t->rows.size(), 1u);
  EXPECT_EQ(t->rows[0], (std::vector<std::string>{"London", "15"}));
}

TEST(BuildResultTable, OrderByDescendingSortsRows) {
  std::vector<std::vector<std::uint8_t>> rows = {
      Row("London", 15), Row("Paris", 20), Row("Berlin", 10)};
  auto stmt = Parse("SELECT city FROM weather ORDER BY temp DESC");
  auto t = BuildResultTable(WeatherSchema(), rows, stmt);
  ASSERT_TRUE(t.ok()) << t.status().message();
  EXPECT_EQ(FirstCells(*t),
            (std::vector<std::string>{"Paris", "London", "Berlin"}));
}

TEST(BuildResultTable, DistinctDropsDuplicateRows) {
  std::vector<std::vector<std::uint8_t>> rows = {
      Row("London", 15), Row("Paris", 20), Row("London", 15)};
  auto stmt = Parse("SELECT DISTINCT city FROM weather ORDER BY city");
  auto t = BuildResultTable(WeatherSchema(), rows, stmt);
  ASSERT_TRUE(t.ok()) << t.status().message();
  EXPECT_EQ(FirstCells(*t), (std::vector<std::string>{"London", "Paris"}));
}

TEST(BuildResultTable, LimitAndOffsetWindowTheRows) {
  std::vector<std::vector<std::uint8_t>> rows = {Row("A", 1), Row("B", 2),
                                                 Row("C", 3), Row("D", 4)};
  auto stmt = Parse("SELECT city FROM weather ORDER BY temp LIMIT 2 OFFSET 1");
  auto t = BuildResultTable(WeatherSchema(), rows, stmt);
  ASSERT_TRUE(t.ok()) << t.status().message();
  EXPECT_EQ(FirstCells(*t), (std::vector<std::string>{"B", "C"}));
}

TEST(BuildResultTable, RejectsOrderByOnAColumnNotReturned) {
  auto stmt = Parse("SELECT city FROM weather ORDER BY nope");
  auto t = BuildResultTable(WeatherSchema(), {Row("London", 15)}, stmt);
  EXPECT_FALSE(t.ok());
}

TEST(BuildResultTable, RejectsProjectingThePrimaryKey) {
  // The key is matched, not stored in the payload, so it cannot be shown.
  auto stmt = Parse("SELECT id FROM weather");
  auto t = BuildResultTable(WeatherSchema(), {Row("London", 15)}, stmt);
  EXPECT_FALSE(t.ok());
}

} // namespace

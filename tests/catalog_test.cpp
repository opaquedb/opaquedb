#include "opaquedb/storage/catalog.h"

#include <filesystem>
#include <string>

#include "opaquedb/config/config.h"
#include "gtest/gtest.h"

namespace opaquedb::storage {
namespace {

namespace fs = std::filesystem;

class CatalogTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("opaquedb_catalog_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  fs::path dir_;
};

TEST_F(CatalogTest, EmptyBaseReportsNoTables) {
  Catalog cat((dir_ / "db").string());
  auto tables = cat.ListTables();
  ASSERT_TRUE(tables.ok()) << tables.status();
  EXPECT_TRUE(tables->empty());
}

TEST_F(CatalogTest, ListsTablesAndDatabasesSortedAndDeduped) {
  fs::create_directories(dir_ / "db" / "sales" / "orders");
  fs::create_directories(dir_ / "db" / "default" / "weather");
  fs::create_directories(dir_ / "db" / "default" / "users");

  Catalog cat((dir_ / "db").string());

  auto tables = cat.ListTables();
  ASSERT_TRUE(tables.ok()) << tables.status();
  ASSERT_EQ(tables->size(), 3u);
  EXPECT_EQ((*tables)[0], (core::TableId{"default", "users"}));
  EXPECT_EQ((*tables)[1], (core::TableId{"default", "weather"}));
  EXPECT_EQ((*tables)[2], (core::TableId{"sales", "orders"}));

  auto dbs = cat.ListDatabases();
  ASSERT_TRUE(dbs.ok()) << dbs.status();
  EXPECT_EQ(*dbs, (std::vector<std::string>{"default", "sales"}));
}

TEST(EpochRoot, IsPerTableUnderDataDir) {
  config::Config cfg;
  cfg.node.data_dir = "/var/lib/opaquedb";
  EXPECT_EQ(cfg.EpochRootFor("default", "weather"),
            "/var/lib/opaquedb/db/default/weather/epochs");
}

} // namespace
} // namespace opaquedb::storage

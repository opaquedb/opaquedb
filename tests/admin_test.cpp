#include "opaquedb/admin/service.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <vector>

#include "opaquedb/admin/keyring_store.h"
#include "opaquedb/config/config.h"
#include "opaquedb/server/ingest.h"
#include "opaquedb/storage/repository.h"

namespace {

namespace fs = std::filesystem;
using opaquedb::admin::AdminService;
using opaquedb::admin::InMemoryKeyringStore;
using opaquedb::config::Config;
using opaquedb::core::Column;
using opaquedb::core::ColumnEncoding;
using opaquedb::core::ColumnType;
using opaquedb::core::Schema;
using opaquedb::core::Value;
using opaquedb::server::BuildStagingFromRows;
using opaquedb::server::IngestRow;
using opaquedb::storage::LocalEpochRepository;

Schema KvSchema() {
  return Schema("t", {Column{"k", ColumnEncoding::kEq, ColumnType::kInt},
                      Column{"v", ColumnEncoding::kRaw, ColumnType::kText}});
}

class AdminFacadeTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("opaquedb_admin_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(dir_);
    cfg_.node.data_dir = dir_.string();
    cfg_.storage.record_bytes = 32;
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  Config cfg_;
  fs::path dir_;
};

TEST_F(AdminFacadeTest, PublishListInspectRollback) {
  auto repo = LocalEpochRepository::Open(cfg_.EffectiveEpochDir());
  ASSERT_TRUE(repo.ok());
  InMemoryKeyringStore keyring;
  AdminService admin(cfg_, repo->get(), &keyring);

  // Nothing published yet.
  auto status0 = admin.GetStatus();
  ASSERT_TRUE(status0.ok());
  EXPECT_FALSE(status0->has_epoch);

  auto schema = KvSchema();
  std::vector<IngestRow> rows = {
      IngestRow{Value{std::int64_t{1}}, {Value{std::string("one")}}},
      IngestRow{Value{std::int64_t{2}}, {Value{std::string("two")}}}};
  auto staging = BuildStagingFromRows(cfg_, schema, rows);
  ASSERT_TRUE(staging.ok()) << staging.status().message();

  auto v1 = admin.Publish(*staging);
  ASSERT_TRUE(v1.ok());
  EXPECT_EQ(*v1, 1u);

  std::vector<IngestRow> rows2 = {
      IngestRow{Value{std::int64_t{3}}, {Value{std::string("three")}}}};
  auto staging2 = BuildStagingFromRows(cfg_, schema, rows2);
  ASSERT_TRUE(staging2.ok());
  auto v2 = admin.Publish(*staging2);
  ASSERT_TRUE(v2.ok());
  EXPECT_EQ(*v2, 2u);

  auto epochs = admin.ListEpochs();
  ASSERT_TRUE(epochs.ok());
  EXPECT_EQ(*epochs, (std::vector<std::uint64_t>{1, 2}));

  auto status = admin.GetStatus();
  ASSERT_TRUE(status.ok());
  EXPECT_TRUE(status->has_epoch);
  EXPECT_EQ(status->epoch_version, 2u);
  EXPECT_EQ(status->row_count, 1u);

  auto inspected = admin.InspectSchema();
  ASSERT_TRUE(inspected.ok());
  EXPECT_EQ(inspected->table(), "t");
  EXPECT_EQ(inspected->columns().size(), 2u);

  ASSERT_TRUE(admin.RollbackEpoch(1).ok());
  auto rolled = admin.GetStatus();
  ASSERT_TRUE(rolled.ok());
  EXPECT_EQ(rolled->epoch_version, 1u);
  EXPECT_EQ(rolled->row_count, 2u);

  EXPECT_FALSE(admin.RollbackEpoch(99).ok()); // unknown version
}

} // namespace

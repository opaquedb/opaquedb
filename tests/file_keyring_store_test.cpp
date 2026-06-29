#include "opaquedb/admin/file_keyring_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "opaquedb/crypto/key_material.h"

// FileKeyringStore is the persistent server keyring: one file per client id,
// surviving restarts so a client registers once. These tests do not need SEAL;
// KeyMaterial is opaque bytes here, which is exactly how the store treats it.

namespace {

namespace fs = std::filesystem;
using opaquedb::admin::FileKeyringStore;
using opaquedb::crypto::KeyMaterial;

KeyMaterial MakeKeys(const std::string &tag) {
  KeyMaterial m;
  m.public_key = "pub-" + tag;
  m.relin_keys = "relin-" + tag;
  m.galois_keys = "galois-" + tag;
  return m;
}

class FileKeyringStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("opaquedb_keyring_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  fs::path dir_;
};

TEST_F(FileKeyringStoreTest, PutGetRoundTrip) {
  auto store = FileKeyringStore::Open(dir_.string());
  ASSERT_TRUE(store.ok()) << store.status().message();

  EXPECT_FALSE((*store)->Contains("alice"));
  ASSERT_TRUE((*store)->Put("alice", MakeKeys("a")).ok());
  EXPECT_TRUE((*store)->Contains("alice"));

  auto got = (*store)->Get("alice");
  ASSERT_TRUE(got.ok()) << got.status().message();
  EXPECT_EQ(got->public_key, "pub-a");
  EXPECT_EQ(got->relin_keys, "relin-a");
  EXPECT_EQ(got->galois_keys, "galois-a");
}

TEST_F(FileKeyringStoreTest, ReregisterOverwritesOneKeysetPerClient) {
  auto store = FileKeyringStore::Open(dir_.string());
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE((*store)->Put("alice", MakeKeys("first")).ok());
  ASSERT_TRUE((*store)->Put("alice", MakeKeys("second")).ok());

  auto got = (*store)->Get("alice");
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got->public_key, "pub-second");

  // One client id, exactly one keyset on disk.
  EXPECT_EQ((*store)->ClientIds().size(), 1u);
}

TEST_F(FileKeyringStoreTest, PersistsAcrossReopen) {
  {
    auto store = FileKeyringStore::Open(dir_.string());
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store)->Put("bob", MakeKeys("b")).ok());
  }
  // A new store over the same directory is a fresh process / a restart. The
  // client must not need to re-register.
  auto reopened = FileKeyringStore::Open(dir_.string());
  ASSERT_TRUE(reopened.ok());
  EXPECT_TRUE((*reopened)->Contains("bob"));
  auto got = (*reopened)->Get("bob");
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got->galois_keys, "galois-b");
}

TEST_F(FileKeyringStoreTest, ClientIdsRoundTripsArbitraryIds) {
  auto store = FileKeyringStore::Open(dir_.string());
  ASSERT_TRUE(store.ok());
  // Ids that are unsafe as raw file names must still round-trip and never
  // escape the directory.
  const std::vector<std::string> ids = {"plain", "a/b", "../escape",
                                        "with space", "tab\tchar"};
  for (const auto &id : ids)
    ASSERT_TRUE((*store)->Put(id, MakeKeys(id)).ok());

  auto listed = (*store)->ClientIds();
  std::sort(listed.begin(), listed.end());
  std::vector<std::string> expected = ids;
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(listed, expected);

  for (const auto &id : ids)
    EXPECT_TRUE((*store)->Contains(id)) << id;

  // No stray files climbed out of the directory.
  EXPECT_FALSE(fs::exists(dir_.parent_path() / "escape"));
}

TEST_F(FileKeyringStoreTest, MissingClientIsNotFound) {
  auto store = FileKeyringStore::Open(dir_.string());
  ASSERT_TRUE(store.ok());
  auto got = (*store)->Get("nobody");
  EXPECT_TRUE(absl::IsNotFound(got.status()));
}

TEST_F(FileKeyringStoreTest, RejectsEmptyClientId) {
  auto store = FileKeyringStore::Open(dir_.string());
  ASSERT_TRUE(store.ok());
  EXPECT_TRUE(absl::IsInvalidArgument((*store)->Put("", MakeKeys("x"))));
}

TEST_F(FileKeyringStoreTest, CorruptFileIsRejectedNotCrashed) {
  auto store = FileKeyringStore::Open(dir_.string());
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE((*store)->Put("carol", MakeKeys("c")).ok());

  // Overwrite the on-disk file with garbage, then read with a fresh store so
  // the in-process cache does not hide the corruption.
  for (const auto &entry : fs::directory_iterator(dir_)) {
    std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
    out << "not a key file";
  }
  auto reopened = FileKeyringStore::Open(dir_.string());
  ASSERT_TRUE(reopened.ok());
  auto got = (*reopened)->Get("carol");
  EXPECT_FALSE(got.ok());
}

} // namespace

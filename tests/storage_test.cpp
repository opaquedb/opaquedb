#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "opaquedb/core/schema.h"
#include "opaquedb/storage/epoch_manifest.h"
#include "opaquedb/storage/repository.h"
#include "opaquedb/storage/segment.h"
#include "opaquedb/storage/staging.h"
#include "opaquedb/storage/wal.h"

namespace {

namespace fs = std::filesystem;
using opaquedb::core::Column;
using opaquedb::core::ColumnEncoding;
using opaquedb::core::Schema;
using opaquedb::storage::EpochReader;
using opaquedb::storage::LocalEpochRepository;
using opaquedb::storage::ParseManifest;
using opaquedb::storage::ReplayWal;
using opaquedb::storage::Row;
using opaquedb::storage::SegmentReader;
using opaquedb::storage::SlotGeometry;
using opaquedb::storage::StagingEpoch;
using opaquedb::storage::WalWriter;
using opaquedb::storage::WriteSegment;

class StorageTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("opaquedb_storage_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
            "_" + std::to_string(counter_++));
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  std::string Path(const std::string &name) const {
    return (dir_ / name).string();
  }

  static StagingEpoch MakeStaging() {
    Schema schema("t", {Column{"k", ColumnEncoding::kEq,
                               opaquedb::core::ColumnType::kInt},
                        Column{"v", ColumnEncoding::kRaw}});
    SlotGeometry geom{/*slot_count=*/16384, /*bytes_per_slot=*/2,
                      /*record_bytes=*/8};
    return StagingEpoch(schema, /*key_bits=*/16, geom);
  }

  static Row MakeRow(std::uint8_t marker) {
    Row row;
    row.match = std::vector<std::uint8_t>(2, marker);         // 2 = ceil(16/8)
    row.payload = std::vector<std::uint8_t>(8, marker + 100); // record_bytes
    return row;
  }

  void Corrupt(const std::string &path, std::size_t offset,
               std::uint8_t value) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(offset));
    f.put(static_cast<char>(value));
  }

  static int counter_;
  fs::path dir_;
};

int StorageTest::counter_ = 0;

TEST_F(StorageTest, SlotGeometryValidates) {
  SlotGeometry geom{16384, 2, 8};
  EXPECT_TRUE(geom.Validate().ok());

  EXPECT_FALSE((SlotGeometry{0, 2, 8}).Validate().ok());     // no slots
  EXPECT_FALSE((SlotGeometry{16384, 0, 8}).Validate().ok()); // no slot width
  EXPECT_FALSE((SlotGeometry{16384, 2, 0}).Validate().ok()); // no record bytes
}

TEST_F(StorageTest, SegmentRoundTrip) {
  std::vector<std::vector<std::uint8_t>> records = {
      {1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
  std::uint32_t crc = 0;
  ASSERT_TRUE(WriteSegment(Path("s.seg"), 4, records, &crc).ok());

  auto reader = SegmentReader::Open(Path("s.seg"), 4, 3, crc);
  ASSERT_TRUE(reader.ok()) << reader.status().message();
  EXPECT_EQ((*reader)->record_count(), 3u);
  for (std::uint64_t i = 0; i < 3; ++i) {
    auto rec = (*reader)->Record(i);
    ASSERT_TRUE(rec.ok());
    EXPECT_TRUE(std::equal(rec->begin(), rec->end(), records[i].begin()));
  }
  EXPECT_FALSE((*reader)->Record(3).ok()); // out of range
}

TEST_F(StorageTest, SegmentRejectsCorruption) {
  std::vector<std::vector<std::uint8_t>> records = {{1, 2, 3, 4}, {5, 6, 7, 8}};
  std::uint32_t crc = 0;
  ASSERT_TRUE(WriteSegment(Path("s.seg"), 4, records, &crc).ok());

  // Bad magic.
  Corrupt(Path("s.seg"), 0, 'X');
  EXPECT_FALSE(SegmentReader::Open(Path("s.seg"), 4, 2, crc).ok());

  // Rewrite, then corrupt a data byte so the CRC fails.
  ASSERT_TRUE(WriteSegment(Path("s.seg"), 4, records, &crc).ok());
  Corrupt(Path("s.seg"), 64, 0xFF);
  EXPECT_FALSE(SegmentReader::Open(Path("s.seg"), 4, 2, crc).ok());

  // Rewrite, then truncate so the declared records run past the file.
  ASSERT_TRUE(WriteSegment(Path("s.seg"), 4, records, &crc).ok());
  std::error_code ec;
  fs::resize_file(Path("s.seg"), 64 + 4, ec); // only room for one record
  ASSERT_FALSE(ec);
  EXPECT_FALSE(SegmentReader::Open(Path("s.seg"), 4, 2, crc).ok());
}

TEST_F(StorageTest, ManifestRoundTripAndRejection) {
  StagingEpoch staging = MakeStaging();
  // Build a manifest by publishing and reading it back through the repository
  // later; here check the parser directly on serialized text.
  opaquedb::storage::EpochManifest m;
  m.epoch_version = 7;
  m.schema = staging.schema();
  m.key_bits = 16;
  m.geometry = staging.geometry();
  m.match_segment = {"match.seg", 2, 3, 111};
  m.payload_segment = {"payload.seg", 8, 3, 222};
  ASSERT_TRUE(m.Validate().ok());

  std::string text = opaquedb::storage::SerializeManifest(m);
  auto parsed = ParseManifest(text);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  EXPECT_EQ(parsed->epoch_version, 7u);
  EXPECT_EQ(parsed->schema.columns().size(), 2u);
  EXPECT_EQ(parsed->match_segment.crc32, 111u);

  EXPECT_FALSE(ParseManifest("{ not json").ok());
  EXPECT_FALSE(ParseManifest("{}").ok()); // missing fields

  // Path traversal in a segment file name must be rejected.
  opaquedb::storage::EpochManifest evil = m;
  evil.match_segment.file = "../escape";
  EXPECT_FALSE(evil.Validate().ok());
}

TEST_F(StorageTest, WalAppendAndReplay) {
  {
    auto wal = WalWriter::Open(Path("wal.log"));
    ASSERT_TRUE(wal.ok());
    for (std::uint8_t i = 0; i < 3; ++i) {
      Row row = MakeRow(i);
      std::vector<std::uint8_t> entry = opaquedb::storage::SerializeRow(row);
      ASSERT_TRUE(wal->Append(entry).ok());
    }
    ASSERT_TRUE(wal->Close().ok());
  }

  auto entries = ReplayWal(Path("wal.log"));
  ASSERT_TRUE(entries.ok());
  ASSERT_EQ(entries->size(), 3u);
  for (std::uint8_t i = 0; i < 3; ++i) {
    auto row = opaquedb::storage::ParseRow((*entries)[i]);
    ASSERT_TRUE(row.ok());
    EXPECT_EQ(row->match, std::vector<std::uint8_t>(2, i));
  }
}

TEST_F(StorageTest, WalReplayToleratesTornTail) {
  {
    auto wal = WalWriter::Open(Path("wal.log"));
    ASSERT_TRUE(wal.ok());
    for (std::uint8_t i = 0; i < 3; ++i) {
      std::vector<std::uint8_t> entry =
          opaquedb::storage::SerializeRow(MakeRow(i));
      ASSERT_TRUE(wal->Append(entry).ok());
    }
  }
  // Chop one byte off the end, as a crash mid-write would leave it.
  std::error_code ec;
  const auto size = fs::file_size(Path("wal.log"), ec);
  ASSERT_FALSE(ec);
  fs::resize_file(Path("wal.log"), size - 1, ec);
  ASSERT_FALSE(ec);

  auto entries = ReplayWal(Path("wal.log"));
  ASSERT_TRUE(entries.ok());
  EXPECT_EQ(entries->size(), 2u) << "torn final entry must be dropped";

  EXPECT_TRUE(ReplayWal(Path("does_not_exist.log")).ok()); // missing -> empty
}

TEST_F(StorageTest, RowParseRejectsMalformed) {
  // A match length of 0xFFFFFFFF with no payload: must not over-read.
  std::vector<std::uint8_t> huge_len = {0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_FALSE(opaquedb::storage::ParseRow(huge_len).ok());
  std::vector<std::uint8_t> empty;
  EXPECT_FALSE(opaquedb::storage::ParseRow(empty).ok());
}

TEST_F(StorageTest, PublishOpenListRollback) {
  auto repo = LocalEpochRepository::Open(Path("repo"));
  ASSERT_TRUE(repo.ok()) << repo.status().message();

  EXPECT_FALSE((*repo)->CurrentVersion().ok()); // nothing published yet

  StagingEpoch v1 = MakeStaging();
  ASSERT_TRUE(v1.AppendRow(MakeRow(1)).ok());
  ASSERT_TRUE(v1.AppendRow(MakeRow(2)).ok());
  ASSERT_TRUE((*repo)->Publish(v1, 1).ok());

  StagingEpoch v2 = MakeStaging();
  ASSERT_TRUE(v2.AppendRow(MakeRow(3)).ok());
  ASSERT_TRUE((*repo)->Publish(v2, 2).ok());

  auto current = (*repo)->CurrentVersion();
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(*current, 2u);

  auto epochs = (*repo)->ListEpochs();
  ASSERT_TRUE(epochs.ok());
  EXPECT_EQ(*epochs, (std::vector<std::uint64_t>{1, 2}));

  // CURRENT reads v2: one row with marker 3.
  auto reader = (*repo)->OpenCurrent();
  ASSERT_TRUE(reader.ok()) << reader.status().message();
  EXPECT_EQ((*reader)->row_count(), 1u);
  auto match = (*reader)->MatchRecord(0);
  ASSERT_TRUE(match.ok());
  EXPECT_EQ(match->front(), 3u);

  // Republishing an existing version is refused: epochs are immutable.
  StagingEpoch again = MakeStaging();
  ASSERT_TRUE(again.AppendRow(MakeRow(9)).ok());
  EXPECT_FALSE((*repo)->Publish(again, 2).ok());

  // Rollback to v1 and confirm CURRENT now reads v1's two rows.
  ASSERT_TRUE((*repo)->Rollback(1).ok());
  auto rolled = (*repo)->OpenCurrent();
  ASSERT_TRUE(rolled.ok());
  EXPECT_EQ((*rolled)->row_count(), 2u);
  EXPECT_FALSE((*repo)->Rollback(99).ok()); // unknown version
}

TEST_F(StorageTest, RecoveryReplaysWalIntoStagingAndPublishes) {
  // Simulate ingest: append rows to a WAL, then crash.
  {
    auto wal = WalWriter::Open(Path("wal.log"));
    ASSERT_TRUE(wal.ok());
    for (std::uint8_t i = 0; i < 4; ++i) {
      ASSERT_TRUE(
          wal->Append(opaquedb::storage::SerializeRow(MakeRow(i))).ok());
    }
  }

  // Restart: replay the WAL to rebuild the staging epoch, then publish it.
  auto entries = ReplayWal(Path("wal.log"));
  ASSERT_TRUE(entries.ok());
  ASSERT_EQ(entries->size(), 4u);

  StagingEpoch staging = MakeStaging();
  for (const auto &entry : *entries) {
    auto row = opaquedb::storage::ParseRow(entry);
    ASSERT_TRUE(row.ok());
    ASSERT_TRUE(staging.AppendRow(*row).ok());
  }

  auto repo = LocalEpochRepository::Open(Path("repo"));
  ASSERT_TRUE(repo.ok());
  ASSERT_TRUE((*repo)->Publish(staging, 1).ok());

  auto reader = (*repo)->OpenCurrent();
  ASSERT_TRUE(reader.ok());
  EXPECT_EQ((*reader)->row_count(), 4u);
  for (std::uint64_t i = 0; i < 4; ++i) {
    auto payload = (*reader)->PayloadRecord(i);
    ASSERT_TRUE(payload.ok());
    EXPECT_EQ(payload->front(), i + 100);
  }
}

} // namespace

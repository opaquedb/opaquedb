#include "opaquedb/storage/epoch_manifest.h"

#include <gtest/gtest.h>

#include <string>

#include "opaquedb/core/schema.h"
#include "opaquedb/storage/staging.h"

// The epoch manifest is machine-written but read back from disk as untrusted
// bytes: a corrupt or hostile manifest must be rejected, never yield a usable
// but wrong epoch (which would steer reads with the wrong geometry or, via a
// crafted segment file name, outside the epoch directory). ParseManifest and
// EpochManifest::Validate are that gate. These tests build a valid manifest,
// confirm it round-trips, then trip each rejection branch one mutation at a
// time so a regression that drops a check is caught here, not in production.

namespace {

using opaquedb::core::Column;
using opaquedb::core::ColumnEncoding;
using opaquedb::core::ColumnType;
using opaquedb::core::Schema;
using opaquedb::storage::EpochManifest;
using opaquedb::storage::MatchRecordBytes;
using opaquedb::storage::ParseManifest;
using opaquedb::storage::SerializeManifest;
using opaquedb::storage::SlotGeometry;

constexpr std::uint32_t kKeyBits = 16;

// A valid manifest: one INT key plus one TEXT secondary index (two searchable
// columns, so the match stride is 2 * ceil(16/8) = 4 bytes) and a raw payload
// column. Geometry record_bytes (32) matches the payload segment stride.
EpochManifest GoodManifest() {
  EpochManifest m;
  m.epoch_version = 7;
  m.schema =
      Schema("weather", {{"id", ColumnEncoding::kEq, ColumnType::kInt},
                         {"city", ColumnEncoding::kIndex, ColumnType::kText},
                         {"note", ColumnEncoding::kRaw, ColumnType::kText}});
  m.key_bits = kKeyBits;
  m.geometry = SlotGeometry{16384, 2, 32};
  const std::uint32_t match_stride = 2 * MatchRecordBytes(kKeyBits); // 4
  m.match_segment = {"match.seg", match_stride, 5, 111};
  m.payload_segment = {"payload.seg", 32, 5, 222};
  return m;
}

TEST(Manifest, ValidAndRoundTrips) {
  EpochManifest m = GoodManifest();
  ASSERT_TRUE(m.Validate().ok()) << "the baseline must be valid";

  auto parsed = ParseManifest(SerializeManifest(m));
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  EXPECT_EQ(parsed->epoch_version, 7u);
  EXPECT_EQ(parsed->key_bits, kKeyBits);
  EXPECT_EQ(parsed->schema.SearchableCount(), 2u);
  EXPECT_EQ(parsed->match_segment.record_bytes, 4u);
}

// --- Validate() rejection branches -----------------------------------------

TEST(Manifest, RejectsUnsupportedFormatVersion) {
  EpochManifest m = GoodManifest();
  m.format_version = 999;
  EXPECT_FALSE(m.Validate().ok());
}

TEST(Manifest, RejectsKeyBitsOutOfRange) {
  EpochManifest m = GoodManifest();
  m.key_bits = 0;
  EXPECT_FALSE(m.Validate().ok());
  m.key_bits = 65;
  EXPECT_FALSE(m.Validate().ok());
}

TEST(Manifest, RejectsInvalidGeometry) {
  EpochManifest m = GoodManifest();
  m.geometry.slot_count = 0;
  EXPECT_FALSE(m.Validate().ok());
}

TEST(Manifest, RejectsMismatchedRowCounts) {
  EpochManifest m = GoodManifest();
  m.payload_segment.record_count = m.match_segment.record_count + 1;
  EXPECT_FALSE(m.Validate().ok()) << "match and payload must agree on rows";
}

TEST(Manifest, RejectsWrongMatchStride) {
  EpochManifest m = GoodManifest();
  // Pretend a single-key stride when there are two searchable columns.
  m.match_segment.record_bytes = MatchRecordBytes(kKeyBits);
  EXPECT_FALSE(m.Validate().ok());
}

TEST(Manifest, RejectsPayloadStrideThatDisagreesWithGeometry) {
  EpochManifest m = GoodManifest();
  m.payload_segment.record_bytes = m.geometry.record_bytes + 8;
  EXPECT_FALSE(m.Validate().ok());
}

TEST(Manifest, RejectsPathTraversalInEitherSegment) {
  EpochManifest a = GoodManifest();
  a.match_segment.file = "../escape";
  EXPECT_FALSE(a.Validate().ok());

  EpochManifest b = GoodManifest();
  b.payload_segment.file = "sub/dir.seg";
  EXPECT_FALSE(b.Validate().ok());

  EpochManifest c = GoodManifest();
  c.payload_segment.file = "";
  EXPECT_FALSE(c.Validate().ok());
}

TEST(Manifest, RejectsAnInvalidEmbeddedSchema) {
  EpochManifest m = GoodManifest();
  // No primary key column -> schema.Validate fails -> manifest fails.
  m.schema = Schema("t", {{"a", ColumnEncoding::kRaw, ColumnType::kInt}});
  EXPECT_FALSE(m.Validate().ok());
}

// --- ParseManifest rejection branches --------------------------------------

TEST(Manifest, ParseRejectsNonJsonAndNonObject) {
  EXPECT_FALSE(ParseManifest("{ not json").ok());
  EXPECT_FALSE(ParseManifest("[]").ok()); // valid JSON, not an object
  EXPECT_FALSE(ParseManifest("42").ok());
}

TEST(Manifest, ParseRejectsMissingTopLevelFields) {
  EXPECT_FALSE(ParseManifest("{}").ok());
  // format_version present but everything else missing.
  EXPECT_FALSE(ParseManifest(R"({"format_version":1})").ok());
}

TEST(Manifest, ParseRejectsWrongTypedField) {
  // Start from a valid document, then corrupt one field's type.
  std::string text = SerializeManifest(GoodManifest());
  // epoch_version is a number; make it a string.
  std::string broken = text;
  auto pos = broken.find("\"epoch_version\": 7");
  ASSERT_NE(pos, std::string::npos);
  broken.replace(pos, std::string("\"epoch_version\": 7").size(),
                 "\"epoch_version\": \"seven\"");
  EXPECT_FALSE(ParseManifest(broken).ok());
}

TEST(Manifest, ParseRejectsNegativeNumber) {
  std::string text = SerializeManifest(GoodManifest());
  auto pos = text.find("\"epoch_version\": 7");
  ASSERT_NE(pos, std::string::npos);
  text.replace(pos, std::string("\"epoch_version\": 7").size(),
               "\"epoch_version\": -1");
  EXPECT_FALSE(ParseManifest(text).ok());
}

TEST(Manifest, ParseRejectsBadEncodingString) {
  std::string text = SerializeManifest(GoodManifest());
  auto pos = text.find("\"eq\"");
  ASSERT_NE(pos, std::string::npos);
  text.replace(pos, 4, "\"xx\"");
  EXPECT_FALSE(ParseManifest(text).ok());
}

TEST(Manifest, ParseRejectsAColumnsFieldThatIsNotAnArray) {
  std::string text = SerializeManifest(GoodManifest());
  auto pos = text.find("\"columns\": [");
  ASSERT_NE(pos, std::string::npos);
  // Replace the opening of the array with an object so it is the wrong type.
  text.replace(pos, std::string("\"columns\": [").size(),
               "\"columns\": {\"x\":[");
  EXPECT_FALSE(ParseManifest(text).ok());
}

} // namespace

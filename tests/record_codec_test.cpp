#include "opaquedb/core/record_codec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "opaquedb/core/schema.h"

// The record codec is the typed payload boundary: EncodeRecord packs a row's
// non-key values at ingest, DecodeRecord unpacks them on the client after
// decryption. DecodeRecord reads on-disk/wire bytes that this code treats as
// untrusted, so a corrupt or hostile record (a text length that runs past the
// buffer, a truncated field) must come back as a status, never an out-of-bounds
// read. These tests drive both directions directly; they are pure and fast.

namespace {

using opaquedb::core::Column;
using opaquedb::core::ColumnEncoding;
using opaquedb::core::ColumnType;
using opaquedb::core::DecodeRecord;
using opaquedb::core::EncodeRecord;
using opaquedb::core::ParseValue;
using opaquedb::core::Schema;
using opaquedb::core::Value;

// A table whose key is an INT (not in payload) and whose payload is one column
// of each type, in schema order: text, int, real.
Schema KvSchema() {
  return Schema("t", {
                         {"id", ColumnEncoding::kEq, ColumnType::kInt},
                         {"city", ColumnEncoding::kRaw, ColumnType::kText},
                         {"pop", ColumnEncoding::kRaw, ColumnType::kInt},
                         {"lat", ColumnEncoding::kRaw, ColumnType::kReal},
                     });
}

TEST(RecordCodec, RoundTripsEveryPayloadType) {
  Schema schema = KvSchema();
  std::vector<Value> payload = {Value{std::string("London")},
                                Value{std::int64_t{8908081}}, Value{51.5072}};

  auto encoded = EncodeRecord(schema, payload, /*record_bytes=*/64);
  ASSERT_TRUE(encoded.ok()) << encoded.status().message();
  EXPECT_EQ(encoded->size(), 64u) << "record is padded to the fixed size";

  auto decoded = DecodeRecord(schema, *encoded);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  ASSERT_EQ(decoded->size(), 3u) << "the kEq key is not part of the payload";
  EXPECT_EQ(std::get<std::string>((*decoded)[0]), "London");
  EXPECT_EQ(std::get<std::int64_t>((*decoded)[1]), 8908081);
  EXPECT_DOUBLE_EQ(std::get<double>((*decoded)[2]), 51.5072);
}

TEST(RecordCodec, EncodeRejectsARecordThatDoesNotFit) {
  Schema schema = KvSchema();
  std::vector<Value> payload = {Value{std::string("a long city name")},
                                Value{std::int64_t{1}}, Value{2.0}};
  // text(2+16) + int(8) + real(8) = 34 bytes, will not fit in 16.
  auto encoded = EncodeRecord(schema, payload, /*record_bytes=*/16);
  EXPECT_FALSE(encoded.ok()) << "an over-long record must be rejected";
}

TEST(RecordCodec, EncodeRejectsTypeMismatch) {
  Schema schema = KvSchema();
  // city is text but handed an int; pop is int but handed text.
  std::vector<Value> wrong_text = {Value{std::int64_t{7}},
                                   Value{std::int64_t{1}}, Value{2.0}};
  EXPECT_FALSE(EncodeRecord(schema, wrong_text, 64).ok());
}

TEST(RecordCodec, EncodeRejectsMissingPayloadValue) {
  Schema schema = KvSchema();
  std::vector<Value> too_few = {Value{std::string("x")}}; // pop, lat missing
  EXPECT_FALSE(EncodeRecord(schema, too_few, 64).ok());
}

TEST(RecordCodec, DecodeRejectsTruncatedFixedWidthColumn) {
  // A single int payload column needs 8 bytes; give it 4.
  Schema schema("t", {{"id", ColumnEncoding::kEq, ColumnType::kInt},
                      {"n", ColumnEncoding::kRaw, ColumnType::kInt}});
  std::vector<std::uint8_t> record(4, 0);
  auto decoded = DecodeRecord(schema, record);
  EXPECT_FALSE(decoded.ok()) << "must not read past a short record";
}

TEST(RecordCodec, DecodeRejectsATextLengthThatRunsPastTheBuffer) {
  // The classic out-of-bounds setup: a 2-byte text length of 0xFFFF with only a
  // few payload bytes after it. The bounds check must reject, not over-read.
  Schema schema("t", {{"id", ColumnEncoding::kEq, ColumnType::kInt},
                      {"s", ColumnEncoding::kRaw, ColumnType::kText}});
  std::vector<std::uint8_t> record = {0xFF, 0xFF, 'h', 'i'}; // len=65535
  auto decoded = DecodeRecord(schema, record);
  EXPECT_FALSE(decoded.ok()) << "an oversized text length must be rejected";
}

TEST(RecordCodec, DecodeRejectsATruncatedTextLengthHeader) {
  // Only one of the text length's two bytes is present.
  Schema schema("t", {{"id", ColumnEncoding::kEq, ColumnType::kInt},
                      {"s", ColumnEncoding::kRaw, ColumnType::kText}});
  std::vector<std::uint8_t> record = {0x01};
  EXPECT_FALSE(DecodeRecord(schema, record).ok());
}

TEST(RecordCodec, DecodeReadsExactlyTheDeclaredTextAndIgnoresPadding) {
  // A well-formed text record with trailing zero padding decodes to just the
  // declared bytes, not the padding.
  Schema schema("t", {{"id", ColumnEncoding::kEq, ColumnType::kInt},
                      {"s", ColumnEncoding::kRaw, ColumnType::kText}});
  std::vector<std::uint8_t> record = {0x03, 0x00, 'a', 'b', 'c', 0, 0, 0};
  auto decoded = DecodeRecord(schema, record);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  ASSERT_EQ(decoded->size(), 1u);
  EXPECT_EQ(std::get<std::string>((*decoded)[0]), "abc");
}

TEST(RecordCodec, ParseValueParsesEachTypeAndRejectsNonNumbers) {
  auto i = ParseValue(ColumnType::kInt, "42");
  ASSERT_TRUE(i.ok());
  EXPECT_EQ(std::get<std::int64_t>(*i), 42);

  auto d = ParseValue(ColumnType::kReal, "3.5");
  ASSERT_TRUE(d.ok());
  EXPECT_DOUBLE_EQ(std::get<double>(*d), 3.5);

  auto t = ParseValue(ColumnType::kText, "anything");
  ASSERT_TRUE(t.ok());
  EXPECT_EQ(std::get<std::string>(*t), "anything");

  EXPECT_FALSE(ParseValue(ColumnType::kInt, "not-an-int").ok());
  EXPECT_FALSE(ParseValue(ColumnType::kReal, "not-a-number").ok());
}

} // namespace

#include "opaquedb/core/key_codec.h"

#include "gtest/gtest.h"

namespace opaquedb::core {
namespace {

constexpr std::uint32_t kKeyBits = 16; // universe 65536

TEST(KeyCodec, IntPassesThroughExactlyAndIsNotLossy) {
  auto enc =
      EncodeKeyValue(ColumnType::kInt, Value{std::int64_t{42}}, kKeyBits);
  ASSERT_TRUE(enc.ok()) << enc.status();
  EXPECT_EQ(enc->value, 42u);
  EXPECT_FALSE(enc->lossy);
}

TEST(KeyCodec, IntBeyondUniverseIsOutOfRange) {
  auto enc =
      EncodeKeyValue(ColumnType::kInt, Value{std::int64_t{65536}}, kKeyBits);
  EXPECT_EQ(enc.status().code(), absl::StatusCode::kOutOfRange);
}

TEST(KeyCodec, IntAtUniverseMaxIsAllowed) {
  auto enc =
      EncodeKeyValue(ColumnType::kInt, Value{std::int64_t{65535}}, kKeyBits);
  ASSERT_TRUE(enc.ok()) << enc.status();
  EXPECT_EQ(enc->value, 65535u);
}

TEST(KeyCodec, TextIsDeterministicWithinUniverseAndLossy) {
  auto a =
      EncodeKeyValue(ColumnType::kText, Value{std::string("London")}, kKeyBits);
  auto b =
      EncodeKeyValue(ColumnType::kText, Value{std::string("London")}, kKeyBits);
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a->value, b->value);
  EXPECT_LT(a->value, 1u << kKeyBits);
  EXPECT_TRUE(a->lossy);

  auto c =
      EncodeKeyValue(ColumnType::kText, Value{std::string("Paris")}, kKeyBits);
  ASSERT_TRUE(c.ok());
  EXPECT_NE(a->value, c->value);
}

TEST(KeyCodec, RealKeyRejected) {
  auto enc = EncodeKeyValue(ColumnType::kReal, Value{3.14}, kKeyBits);
  EXPECT_EQ(enc.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(KeyCodec, TypeMismatchRejected) {
  auto enc =
      EncodeKeyValue(ColumnType::kInt, Value{std::string("x")}, kKeyBits);
  EXPECT_EQ(enc.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(KeyCodec, KeyToBitsIsLittleEndian) {
  auto bits = KeyToBits(0b1011, 8);
  ASSERT_EQ(bits.size(), 8u);
  EXPECT_EQ(bits[0], 1u);
  EXPECT_EQ(bits[1], 1u);
  EXPECT_EQ(bits[2], 0u);
  EXPECT_EQ(bits[3], 1u);
  EXPECT_EQ(bits[4], 0u);
}

TEST(KeyCodec, PackUnpackRoundTrips) {
  for (std::uint64_t v : {0u, 1u, 255u, 256u, 65535u}) {
    auto packed = PackKey(v, kKeyBits);
    EXPECT_EQ(packed.size(), KeyRecordBytes(kKeyBits));
    auto back = UnpackKey(packed, kKeyBits);
    ASSERT_TRUE(back.ok()) << back.status();
    EXPECT_EQ(*back, v);
  }
}

TEST(KeyCodec, UnpackRejectsWrongLength) {
  std::vector<std::uint8_t> wrong(KeyRecordBytes(kKeyBits) + 1, 0);
  auto back = UnpackKey(wrong, kKeyBits);
  EXPECT_EQ(back.status().code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace opaquedb::core

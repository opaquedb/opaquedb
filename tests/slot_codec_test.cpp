#include "opaquedb/core/slot_codec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

// The slot codec packs a record's payload bytes into plaintext slot integers
// and back. The matcher relies on one invariant: unpacking the packed slots
// reproduces the original bytes exactly, for any record size and any legal
// bytes_per_slot, including sizes that do not divide evenly (the last slot is
// partially filled) and the maximum 7 bytes per slot. These are functional
// tests of that invariant: they sweep the awkward sizes a single fixed-size
// example would never reach, so a packing off-by-one or a dropped tail byte
// shows up as a round-trip mismatch rather than as silently corrupted payloads
// in a live query.

namespace {

using opaquedb::core::PackBytesToSlots;
using opaquedb::core::UnpackSlotsToBytes;

// A deterministic byte pattern with all 256 values, so a byte that lands in the
// wrong slot position changes the result.
std::vector<std::uint8_t> Pattern(std::size_t n) {
  std::vector<std::uint8_t> b(n);
  for (std::size_t i = 0; i < n; ++i)
    b[i] = static_cast<std::uint8_t>((i * 37 + 11) & 0xFF);
  return b;
}

TEST(SlotCodec, RoundTripsAcrossAllSizesAndSlotWidths) {
  // Sweep record sizes around every slot boundary and every legal slot width.
  for (std::uint32_t bps = 1; bps <= 7; ++bps) {
    for (std::uint32_t bytes = 0; bytes <= 64; ++bytes) {
      std::vector<std::uint8_t> data = Pattern(bytes);
      auto slots = PackBytesToSlots(data, bps);
      ASSERT_TRUE(slots.ok())
          << "pack failed at bytes=" << bytes << " bps=" << bps;
      auto back = UnpackSlotsToBytes(*slots, bytes, bps);
      ASSERT_TRUE(back.ok())
          << "unpack failed at bytes=" << bytes << " bps=" << bps;
      EXPECT_EQ(*back, data)
          << "round-trip mismatch at bytes=" << bytes << " bps=" << bps;
    }
  }
}

TEST(SlotCodec, PackUsesTheMinimumNumberOfSlots) {
  // ceil(bytes / bytes_per_slot) slots, no more: a stray empty slot would waste
  // a plane and shift every downstream offset.
  for (std::uint32_t bps = 1; bps <= 7; ++bps) {
    for (std::uint32_t bytes = 1; bytes <= 40; ++bytes) {
      auto slots = PackBytesToSlots(Pattern(bytes), bps);
      ASSERT_TRUE(slots.ok());
      EXPECT_EQ(slots->size(), (bytes + bps - 1) / bps)
          << "slot count wrong at bytes=" << bytes << " bps=" << bps;
    }
  }
}

TEST(SlotCodec, RejectsIllegalSlotWidths) {
  std::vector<std::uint8_t> data = Pattern(8);
  EXPECT_FALSE(PackBytesToSlots(data, 0).ok());
  EXPECT_FALSE(PackBytesToSlots(data, 8).ok()); // over the 7-byte cap
  EXPECT_FALSE(UnpackSlotsToBytes({}, 8, 0).ok());
  EXPECT_FALSE(UnpackSlotsToBytes({}, 8, 8).ok());
}

TEST(SlotCodec, UnpackRejectsTooFewSlots) {
  // Asking to unpack more bytes than the slots can hold must fail, not read
  // past the slot span.
  auto slots = PackBytesToSlots(Pattern(4), /*bytes_per_slot=*/2); // 2 slots
  ASSERT_TRUE(slots.ok());
  ASSERT_EQ(slots->size(), 2u);
  EXPECT_FALSE(UnpackSlotsToBytes(*slots, /*record_bytes=*/40, 2).ok());
}

} // namespace

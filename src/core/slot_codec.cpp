#include "opaquedb/core/slot_codec.h"

#include "absl/strings/str_cat.h"

namespace opaquedb::core {
namespace {

constexpr std::uint32_t kMaxBytesPerSlot = 7;

} // namespace

std::vector<int> RequiredGaloisSteps(std::uint32_t slot_count,
                                     std::uint32_t key_bits) {
  std::vector<int> steps;
  if (key_bits == 0 || slot_count < 2)
    return steps;
  const std::uint32_t row = slot_count / 2;
  // AND reduction (left) and masked broadcast (right) within a block.
  for (std::uint32_t s = 1; s < key_bits; s <<= 1) {
    steps.push_back(static_cast<int>(s));
    steps.push_back(-static_cast<int>(s));
  }
  // Block-sum across blocks within a row (left), powers of two from key_bits
  // up.
  for (std::uint32_t s = key_bits; s < row; s <<= 1)
    steps.push_back(static_cast<int>(s));
  return steps;
}

absl::StatusOr<std::vector<std::uint64_t>>
PackBytesToSlots(std::span<const std::uint8_t> bytes,
                 std::uint32_t bytes_per_slot) {
  if (bytes_per_slot == 0 || bytes_per_slot > kMaxBytesPerSlot) {
    return absl::InvalidArgumentError(absl::StrCat(
        "bytes_per_slot must be between 1 and ", kMaxBytesPerSlot));
  }
  std::vector<std::uint64_t> slots;
  slots.reserve((bytes.size() + bytes_per_slot - 1) / bytes_per_slot);
  std::size_t i = 0;
  while (i < bytes.size()) {
    std::uint64_t value = 0;
    for (std::uint32_t b = 0; b < bytes_per_slot && i < bytes.size();
         ++b, ++i) {
      value |= static_cast<std::uint64_t>(bytes[i]) << (8 * b);
    }
    slots.push_back(value);
  }
  return slots;
}

absl::StatusOr<std::vector<std::uint8_t>>
UnpackSlotsToBytes(std::span<const std::uint64_t> slots,
                   std::uint32_t record_bytes, std::uint32_t bytes_per_slot) {
  if (bytes_per_slot == 0 || bytes_per_slot > kMaxBytesPerSlot) {
    return absl::InvalidArgumentError(absl::StrCat(
        "bytes_per_slot must be between 1 and ", kMaxBytesPerSlot));
  }
  const std::uint32_t needed_slots =
      (record_bytes + bytes_per_slot - 1) / bytes_per_slot;
  if (slots.size() < needed_slots) {
    return absl::InvalidArgumentError(
        absl::StrCat("have ", slots.size(), " slots, need ", needed_slots,
                     " to unpack ", record_bytes, " bytes"));
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(record_bytes);
  std::uint32_t produced = 0;
  for (std::uint32_t s = 0; s < needed_slots && produced < record_bytes; ++s) {
    std::uint64_t value = slots[s];
    for (std::uint32_t b = 0; b < bytes_per_slot && produced < record_bytes;
         ++b, ++produced) {
      bytes.push_back(static_cast<std::uint8_t>((value >> (8 * b)) & 0xFFu));
    }
  }
  return bytes;
}

} // namespace opaquedb::core

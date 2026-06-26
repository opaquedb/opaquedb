#ifndef OPAQUEDB_CORE_SLOT_CODEC_H_
#define OPAQUEDB_CORE_SLOT_CODEC_H_

#include <cstdint>
#include <span>
#include <vector>

#include "absl/status/statusor.h"

// Packs payload bytes into BatchEncoder slot values and back. This lives in
// core because both the server (when laying out a payload for the masked
// retrieve) and the client (when reading a decrypted result) use it, and it
// touches no SEAL types. Each slot holds bytes_per_slot bytes as a
// little-endian integer, so the value stays below the plaintext modulus when
// bytes_per_slot is chosen for the configured modulus.

namespace opaquedb::core {

// Packs record bytes into ceil(len / bytes_per_slot) slot values.
// bytes_per_slot must be between 1 and 7 so the packed value fits a 64-bit slot
// with room under the plaintext modulus.
absl::StatusOr<std::vector<std::uint64_t>>
PackBytesToSlots(std::span<const std::uint8_t> bytes,
                 std::uint32_t bytes_per_slot);

// Unpacks the first ceil(record_bytes / bytes_per_slot) slots back into exactly
// record_bytes bytes. Extra trailing slots are ignored.
absl::StatusOr<std::vector<std::uint8_t>>
UnpackSlotsToBytes(std::span<const std::uint64_t> slots,
                   std::uint32_t record_bytes, std::uint32_t bytes_per_slot);

// The bit-sliced matcher lays each record's payload across key_bits slots per
// plane (a "block"), so one block holds key_bits * bytes_per_slot payload
// bytes. A record that does not fit in one block is split across this many
// planes, each retrieved into its own result ciphertext.
inline std::uint32_t PayloadBytesPerBlock(std::uint32_t key_bits,
                                          std::uint32_t bytes_per_slot) {
  return key_bits * bytes_per_slot;
}

inline std::uint32_t PayloadPlaneCount(std::uint32_t record_bytes,
                                       std::uint32_t key_bits,
                                       std::uint32_t bytes_per_slot) {
  const std::uint32_t per = PayloadBytesPerBlock(key_bits, bytes_per_slot);
  return per == 0 ? 0 : (record_bytes + per - 1) / per;
}

// The slot distance between adjacent result buckets within one BatchEncoder
// row. The multi-match matcher stops the block-sum early, leaving `buckets`
// partial sums per row spaced this far apart; bucket g is read at slot
// g * BucketStride in row 0 and at row + g * BucketStride in row 1, each one
// key_bits-wide block. With buckets == 1 the stride is the whole row, which is
// the single-match path (bucket 0 holds the sum of every block). buckets must
// be a power of two and at most slot_count / 2 / key_bits.
inline std::uint32_t BucketStride(std::uint32_t slot_count,
                                  std::uint32_t buckets) {
  const std::uint32_t row = slot_count / 2;
  return buckets == 0 ? row : row / buckets;
}

// The exact set of Galois rotation steps the bit-sliced matcher needs, so the
// client generates only these keys instead of the full power-of-two set (which
// is hundreds of MB at poly 16384). The matcher rotates by:
//   +/- 2^i for i in [0, log2(key_bits))  (AND reduction and masked broadcast)
//   + 2^i for i in [log2(key_bits), log2(slot_count/2))  (block-sum within a
//   row)
// It uses no column rotation: the two slot rows are summed on the client. This
// must stay in sync with the rotations in the reference backend's evaluate.
std::vector<int> RequiredGaloisSteps(std::uint32_t slot_count,
                                     std::uint32_t key_bits);

} // namespace opaquedb::core

#endif // OPAQUEDB_CORE_SLOT_CODEC_H_

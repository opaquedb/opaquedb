#ifndef OPAQUEDB_CORE_KEY_CODEC_H_
#define OPAQUEDB_CORE_KEY_CODEC_H_

#include <cstdint>
#include <span>
#include <vector>

#include "absl/status/statusor.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/core/schema.h"

// Maps a typed match-key value to the integer the bit-sliced matcher compares.
// This is the single source of truth for "what number does this key become",
// shared by ingest (server) and the query client so the two sides always agree.
// It lives in core because it is pure value mapping; it knows nothing about
// SEAL or storage.
//
// The match key can be any column type the schema allows as a key:
//   INT  -> the value itself, reinterpreted as unsigned. Must fit in key_bits
//           bits so the match is exact.
//   TEXT -> a deterministic 64-bit hash of the UTF-8 bytes, reduced into the
//           key_bits universe. A hash can collide, so a TEXT key match is only
//           a candidate; a larger key_bits lowers the collision rate but
//           correctness never depends on it.
//   REAL -> rejected. Equality on floating point is not well defined here.
//
// The matcher binary-expands the key value into key_bits SIMD slots per record
// (see KeyToBits). Storage holds the key value itself, packed little-endian
// into KeyRecordBytes(key_bits) bytes.

namespace opaquedb::core {

struct KeyEncoding {
  std::uint64_t value; // the integer the matcher binary-expands
  bool lossy;          // true when a hash was used; the match is a candidate
};

// Maps the key value of a column to its matcher integer in the 2^key_bits
// universe. Fails if the column type cannot be a key, the value does not match
// the column type, or an INT value does not fit in key_bits bits.
absl::StatusOr<KeyEncoding> EncodeKeyValue(ColumnType type, const Value &value,
                                           std::uint32_t key_bits);

// Bytes a stored key value occupies: ceil(key_bits / 8), little-endian.
inline std::uint32_t KeyRecordBytes(std::uint32_t key_bits) {
  return (key_bits + 7) / 8;
}

// The mask covering the key_bits universe. key_bits must be in [1, 64].
inline std::uint64_t KeyMask(std::uint32_t key_bits) {
  return key_bits >= 64 ? ~0ull : ((1ull << key_bits) - 1);
}

// Binary-expands a key value into key_bits bits, least significant bit first.
// Each element is 0 or 1. This is the per-record slot pattern the matcher lays
// down for a stored key and the client broadcasts for the query value.
std::vector<std::uint64_t> KeyToBits(std::uint64_t value,
                                     std::uint32_t key_bits);

// Packs a key value into KeyRecordBytes(key_bits) little-endian bytes for
// storage, and unpacks it back. Unpack validates the byte length.
std::vector<std::uint8_t> PackKey(std::uint64_t value, std::uint32_t key_bits);
absl::StatusOr<std::uint64_t> UnpackKey(std::span<const std::uint8_t> bytes,
                                        std::uint32_t key_bits);

} // namespace opaquedb::core

#endif // OPAQUEDB_CORE_KEY_CODEC_H_

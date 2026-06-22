#ifndef OPAQUEDB_STORAGE_LITTLE_ENDIAN_H_
#define OPAQUEDB_STORAGE_LITTLE_ENDIAN_H_

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Explicit little-endian encode and decode for on-disk integers. We never
// reinterpret_cast a struct over file bytes: padding and host endianness make
// that undefined and non-portable, and over untrusted bytes it is a
// memory-safety hazard. Reads are bounds-checked and return nullopt when the
// span is too short.

namespace opaquedb::storage {

inline void AppendU32LE(std::vector<std::uint8_t> &out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

inline void AppendU64LE(std::vector<std::uint8_t> &out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

// Reads a u32 at offset. Returns nullopt if the four bytes are not present.
inline std::optional<std::uint32_t>
ReadU32LE(std::span<const std::uint8_t> data, std::size_t offset) {
  if (offset > data.size() || data.size() - offset < 4)
    return std::nullopt;
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

inline std::optional<std::uint64_t>
ReadU64LE(std::span<const std::uint8_t> data, std::size_t offset) {
  if (offset > data.size() || data.size() - offset < 8)
    return std::nullopt;
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |=
        static_cast<std::uint64_t>(data[offset + static_cast<std::size_t>(i)])
        << (8 * i);
  }
  return value;
}

} // namespace opaquedb::storage

#endif // OPAQUEDB_STORAGE_LITTLE_ENDIAN_H_

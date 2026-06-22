#include "crc32.h"

#include <array>

namespace opaquedb::storage {
namespace {

// The standard reflected CRC32 table, built once on first use.
std::array<std::uint32_t, 256> BuildTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    table[i] = crc;
  }
  return table;
}

} // namespace

std::uint32_t Crc32(std::span<const std::uint8_t> data) {
  static const std::array<std::uint32_t, 256> table = BuildTable();
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::uint8_t byte : data) {
    crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

} // namespace opaquedb::storage

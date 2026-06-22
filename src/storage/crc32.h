#ifndef OPAQUEDB_STORAGE_CRC32_H_
#define OPAQUEDB_STORAGE_CRC32_H_

#include <cstdint>
#include <span>

// CRC32 (IEEE 802.3) for corruption detection in segment files and the WAL.
// This guards against accidental corruption and torn writes. It is not a MAC
// and provides no protection against deliberate tampering.

namespace opaquedb::storage {

std::uint32_t Crc32(std::span<const std::uint8_t> data);

} // namespace opaquedb::storage

#endif // OPAQUEDB_STORAGE_CRC32_H_

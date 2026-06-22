#ifndef OPAQUEDB_STORAGE_CHECKED_H_
#define OPAQUEDB_STORAGE_CHECKED_H_

#include <cstdint>
#include <optional>

// Overflow-checked size arithmetic. On-disk and client-supplied sizes are
// untrusted, so any product or sum used to bound a read must not wrap. These
// return nullopt on overflow; callers turn that into a status.

namespace opaquedb::storage {

inline std::optional<std::uint64_t> CheckedMul(std::uint64_t a,
                                               std::uint64_t b) {
  std::uint64_t result = 0;
  if (__builtin_mul_overflow(a, b, &result))
    return std::nullopt;
  return result;
}

inline std::optional<std::uint64_t> CheckedAdd(std::uint64_t a,
                                               std::uint64_t b) {
  std::uint64_t result = 0;
  if (__builtin_add_overflow(a, b, &result))
    return std::nullopt;
  return result;
}

} // namespace opaquedb::storage

#endif // OPAQUEDB_STORAGE_CHECKED_H_

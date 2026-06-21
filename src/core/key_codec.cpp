#include "opaquedb/core/key_codec.h"

#include <cstdint>
#include <string>

#include "absl/strings/str_cat.h"

namespace opaquedb::core {
namespace {

// FNV-1a over the bytes. A fixed, dependency-free hash so ingest and the client
// derive the same integer for the same key across processes and runs.
// absl::Hash is unsuitable here because it is salted per process.
std::uint64_t Fnv1a(const std::string &bytes) {
  std::uint64_t hash = 0xcbf29ce484222325ull;
  for (unsigned char c : bytes) {
    hash ^= c;
    hash *= 0x100000001b3ull;
  }
  return hash;
}

} // namespace

absl::StatusOr<KeyEncoding> EncodeKeyValue(ColumnType type, const Value &value,
                                           std::uint32_t key_bits) {
  const std::uint64_t mask = KeyMask(key_bits);
  switch (type) {
  case ColumnType::kInt: {
    const auto *i = std::get_if<std::int64_t>(&value);
    if (i == nullptr)
      return absl::InvalidArgumentError("match key expects an integer value");
    const auto u = static_cast<std::uint64_t>(*i);
    if (key_bits < 64 && u > mask) {
      return absl::OutOfRangeError(
          absl::StrCat("int key ", *i, " does not fit in ", key_bits,
                       " bits (max ", mask, "); raise crypto.key_bits"));
    }
    return KeyEncoding{u, /*lossy=*/false};
  }
  case ColumnType::kText: {
    const auto *s = std::get_if<std::string>(&value);
    if (s == nullptr)
      return absl::InvalidArgumentError("match key expects a text value");
    return KeyEncoding{Fnv1a(*s) & mask, /*lossy=*/true};
  }
  case ColumnType::kReal:
    return absl::InvalidArgumentError(
        "a REAL column cannot be a match key; equality on floating point is "
        "not well defined");
  }
  return absl::InternalError("unknown column type");
}

std::vector<std::uint64_t> KeyToBits(std::uint64_t value,
                                     std::uint32_t key_bits) {
  std::vector<std::uint64_t> bits(key_bits);
  for (std::uint32_t b = 0; b < key_bits; ++b)
    bits[b] = (value >> b) & 1ull;
  return bits;
}

std::vector<std::uint8_t> PackKey(std::uint64_t value, std::uint32_t key_bits) {
  const std::uint32_t n = KeyRecordBytes(key_bits);
  std::vector<std::uint8_t> out(n);
  for (std::uint32_t i = 0; i < n; ++i)
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  return out;
}

absl::StatusOr<std::uint64_t> UnpackKey(std::span<const std::uint8_t> bytes,
                                        std::uint32_t key_bits) {
  const std::uint32_t n = KeyRecordBytes(key_bits);
  if (bytes.size() != n) {
    return absl::InvalidArgumentError(
        absl::StrCat("key record is ", bytes.size(), " bytes, expected ", n));
  }
  std::uint64_t value = 0;
  for (std::uint32_t i = 0; i < n; ++i)
    value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
  return value & KeyMask(key_bits);
}

} // namespace opaquedb::core

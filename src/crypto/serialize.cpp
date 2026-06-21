#include "opaquedb/crypto/serialize.h"

#include <cstring>
#include <exception>
#include <sstream>

#include "absl/strings/str_cat.h"

namespace opaquedb::crypto {
namespace {

void AppendU32(std::string &out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
  }
}

// Reads a little-endian u32 at offset, advancing it. Returns false if the four
// bytes are not available.
bool ReadU32(const std::string &bytes, std::size_t &offset,
             std::uint32_t *out) {
  if (offset > bytes.size() || bytes.size() - offset < 4)
    return false;
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes[offset + i]))
             << (8 * i);
  }
  offset += 4;
  *out = value;
  return true;
}

} // namespace

std::string SerializeCiphertexts(const std::vector<seal::Ciphertext> &ciphers) {
  std::string out;
  AppendU32(out, static_cast<std::uint32_t>(ciphers.size()));
  for (const seal::Ciphertext &cipher : ciphers) {
    std::ostringstream stream(std::ios::binary);
    cipher.save(stream);
    const std::string blob = stream.str();
    AppendU32(out, static_cast<std::uint32_t>(blob.size()));
    out.append(blob);
  }
  return out;
}

absl::StatusOr<std::vector<seal::Ciphertext>>
DeserializeCiphertexts(const CryptoContext &ctx, const std::string &bytes,
                       std::uint32_t max_count) {
  std::size_t offset = 0;
  std::uint32_t count = 0;
  if (!ReadU32(bytes, offset, &count)) {
    return absl::InvalidArgumentError("ciphertext list: truncated count");
  }
  if (count > max_count) {
    return absl::InvalidArgumentError(
        absl::StrCat("ciphertext list declares ", count,
                     " items, over the limit ", max_count));
  }

  std::vector<seal::Ciphertext> ciphers;
  ciphers.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t length = 0;
    if (!ReadU32(bytes, offset, &length)) {
      return absl::InvalidArgumentError(
          absl::StrCat("ciphertext ", i, ": truncated length"));
    }
    if (length > bytes.size() - offset) {
      return absl::InvalidArgumentError(
          absl::StrCat("ciphertext ", i, ": length runs past the buffer"));
    }
    // load() validates the blob against the context, including its parms_id, so
    // a ciphertext built for different parameters is rejected here.
    try {
      seal::Ciphertext cipher;
      std::istringstream stream(bytes.substr(offset, length), std::ios::binary);
      cipher.load(ctx.seal(), stream);
      // Defense in depth over load()'s own checks: a client operand is a fresh
      // encryption, so it must sit at the top of the modulus chain and be a
      // normal two-polynomial ciphertext. Reject anything else before it
      // reaches the evaluator, where an unexpected shape could be costly or
      // unsound.
      if (cipher.parms_id() != ctx.seal().first_parms_id()) {
        return absl::InvalidArgumentError(absl::StrCat(
            "ciphertext ", i, " has unexpected encryption parameters"));
      }
      if (cipher.size() < 2 || cipher.size() > 3) {
        return absl::InvalidArgumentError(absl::StrCat(
            "ciphertext ", i, " has unexpected size ", cipher.size()));
      }
      ciphers.push_back(std::move(cipher));
    } catch (const std::exception &e) {
      return absl::InvalidArgumentError(
          absl::StrCat("ciphertext ", i, " rejected: ", e.what()));
    }
    offset += length;
  }
  if (offset != bytes.size()) {
    return absl::InvalidArgumentError("ciphertext list has trailing bytes");
  }
  return ciphers;
}

} // namespace opaquedb::crypto

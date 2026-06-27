#include "opaquedb/crypto/ops.h"

#include <exception>
#include <optional>
#include <span>
#include <vector>

#include "absl/strings/str_cat.h"
#include "opaquedb/core/key_codec.h"
#include "opaquedb/core/slot_codec.h"
#include "opaquedb/crypto/serialize.h"

namespace opaquedb::crypto {

absl::StatusOr<seal::Ciphertext> Encrypt(const CryptoContext &ctx,
                                         const seal::PublicKey &public_key,
                                         const seal::Plaintext &plain) {
  try {
    seal::Encryptor encryptor(ctx.seal(), public_key);
    seal::Ciphertext cipher;
    encryptor.encrypt(plain, cipher);
    return cipher;
  } catch (const std::exception &e) {
    return absl::InvalidArgumentError(
        absl::StrCat("encrypt failed: ", e.what()));
  }
}

absl::StatusOr<seal::Plaintext> Decrypt(const CryptoContext &ctx,
                                        const seal::SecretKey &secret_key,
                                        const seal::Ciphertext &cipher) {
  try {
    seal::Decryptor decryptor(ctx.seal(), secret_key);
    seal::Plaintext plain;
    decryptor.decrypt(cipher, plain);
    return plain;
  } catch (const std::exception &e) {
    return absl::InvalidArgumentError(
        absl::StrCat("decrypt failed: ", e.what()));
  }
}

absl::StatusOr<int> NoiseBudgetBits(const CryptoContext &ctx,
                                    const seal::SecretKey &secret_key,
                                    const seal::Ciphertext &cipher) {
  try {
    seal::Decryptor decryptor(ctx.seal(), secret_key);
    return decryptor.invariant_noise_budget(cipher);
  } catch (const std::exception &e) {
    return absl::InvalidArgumentError(
        absl::StrCat("noise budget query failed: ", e.what()));
  }
}

namespace {

// Encrypts one lookup value into the tiled operand ciphertext: the value's bits
// repeated across every slot so each record's block of key_bits slots compares
// against the same value (slot i holds bit i % key_bits).
absl::StatusOr<seal::Ciphertext> EncryptOperand(const CryptoContext &ctx,
                                                const seal::PublicKey &pub,
                                                std::uint64_t value,
                                                std::uint32_t key_bits) {
  const std::vector<std::uint64_t> bits = core::KeyToBits(value, key_bits);
  std::vector<std::uint64_t> slots(ctx.slot_count());
  for (std::size_t i = 0; i < slots.size(); ++i)
    slots[i] = bits[i % key_bits];
  absl::StatusOr<seal::Plaintext> plain = ctx.EncodeBatch(slots);
  if (!plain.ok())
    return plain.status();
  return Encrypt(ctx, pub, *plain);
}

} // namespace

absl::StatusOr<std::string> BuildEncryptedOperand(const CryptoContext &ctx,
                                                  const seal::PublicKey &pub,
                                                  std::uint64_t value,
                                                  std::uint32_t key_bits) {
  return BuildEncryptedOperands(ctx, pub, {value}, key_bits);
}

absl::StatusOr<std::string>
BuildEncryptedOperands(const CryptoContext &ctx, const seal::PublicKey &pub,
                       const std::vector<std::uint64_t> &values,
                       std::uint32_t key_bits) {
  if (key_bits == 0 || key_bits > 64) {
    return absl::InvalidArgumentError("key_bits must be in [1, 64]");
  }
  if (values.empty()) {
    return absl::InvalidArgumentError("operand list is empty");
  }
  std::vector<seal::Ciphertext> ciphers;
  ciphers.reserve(values.size());
  for (std::uint64_t value : values) {
    absl::StatusOr<seal::Ciphertext> cipher =
        EncryptOperand(ctx, pub, value, key_bits);
    if (!cipher.ok())
      return cipher.status();
    ciphers.push_back(*std::move(cipher));
  }
  return SerializeCiphertexts(ciphers);
}

absl::StatusOr<std::vector<BucketResult>>
DecryptResults(const CryptoContext &ctx, const seal::SecretKey &sk,
               const std::string &encrypted_blob, std::uint32_t key_bits,
               std::uint32_t record_bytes, std::uint32_t bytes_per_slot,
               std::uint32_t buckets, std::uint64_t offset,
               std::uint64_t limit) {
  if (buckets == 0) {
    return absl::InvalidArgumentError("buckets must be positive");
  }
  const std::uint32_t planes =
      core::PayloadPlaneCount(record_bytes, key_bits, bytes_per_slot);
  // planes payload ciphertexts plus one trailing presence ciphertext.
  const std::uint32_t expected = planes + 1;
  absl::StatusOr<std::vector<seal::Ciphertext>> ciphers = DeserializeCiphertexts(
      ctx, encrypted_blob, /*max_count=*/expected, /*require_top_level=*/false);
  if (!ciphers.ok())
    return ciphers.status();
  if (ciphers->size() != expected) {
    return absl::InternalError(absl::StrCat(
        "result has ", ciphers->size(), " ciphertexts, expected ", expected));
  }

  // Decrypt and decode every ciphertext once into slot vectors.
  std::vector<std::vector<std::uint64_t>> slots(expected);
  std::size_t row = 0;
  for (std::uint32_t c = 0; c < expected; ++c) {
    absl::StatusOr<seal::Plaintext> plain = Decrypt(ctx, sk, (*ciphers)[c]);
    if (!plain.ok())
      return plain.status();
    absl::StatusOr<std::vector<std::uint64_t>> decoded =
        ctx.DecodeBatch(*plain);
    if (!decoded.ok())
      return decoded.status();
    slots[c] = *std::move(decoded);
    if (c == 0)
      row = slots[c].size() / 2;
  }
  if (row < key_bits) {
    return absl::InternalError("decoded plane has fewer slots than key_bits");
  }
  if (row % buckets != 0) {
    return absl::InternalError(absl::StrCat(
        "slot row ", row, " is not divisible by buckets ", buckets));
  }
  const std::size_t stride = row / buckets;

  // Reads one bucket's first block (key_bits slots) from a decoded ciphertext,
  // summing the two slot rows: the matched payload lands in the bucket's block
  // of whichever row holds it, and the other row's is zero.
  auto bucket_block = [&](std::uint32_t cipher, std::size_t base) {
    std::vector<std::uint64_t> block(key_bits);
    const std::vector<std::uint64_t> &s = slots[cipher];
    for (std::uint32_t i = 0; i < key_bits; ++i)
      block[i] = s[base + i] + s[row + base + i];
    return block;
  };

  const std::uint32_t bytes_per_plane =
      core::PayloadBytesPerBlock(key_bits, bytes_per_slot);

  // Clamp the [offset, offset + limit) window to the bucket count without
  // overflowing: at most `buckets - start` buckets remain after the offset.
  const std::uint64_t start = offset < buckets ? offset : buckets;
  const std::uint64_t remaining = buckets - start;
  const std::uint64_t window = limit < remaining ? limit : remaining;
  const std::uint64_t stop = start + window;
  std::vector<BucketResult> out;
  out.reserve(static_cast<std::size_t>(window));
  for (std::uint64_t g = start; g < stop; ++g) {
    const std::size_t base = static_cast<std::size_t>(g) * stride;
    const std::vector<std::uint64_t> pres = bucket_block(planes, base);
    BucketResult br;
    br.count = pres[0]; // matches in this bucket; for COUNT(*) the client sums
    if (pres[0] == 0) {
      out.push_back(std::move(br)); // empty bucket
      continue;
    }
    br.present = true;
    if (pres[0] >= 2) {
      br.collided = true; // multiple rows summed together: drop the bytes
      out.push_back(std::move(br));
      continue;
    }
    br.record.reserve(static_cast<std::size_t>(planes) * bytes_per_plane);
    for (std::uint32_t p = 0; p < planes; ++p) {
      const std::vector<std::uint64_t> block = bucket_block(p, base);
      absl::StatusOr<std::vector<std::uint8_t>> chunk =
          core::UnpackSlotsToBytes(block, bytes_per_plane, bytes_per_slot);
      if (!chunk.ok())
        return chunk.status();
      br.record.insert(br.record.end(), chunk->begin(), chunk->end());
    }
    br.record.resize(record_bytes);
    out.push_back(std::move(br));
  }
  return out;
}

absl::StatusOr<std::optional<std::vector<std::uint8_t>>>
DecryptRecord(const CryptoContext &ctx, const seal::SecretKey &sk,
              const std::string &encrypted_blob, std::uint32_t key_bits,
              std::uint32_t record_bytes, std::uint32_t bytes_per_slot) {
  absl::StatusOr<std::vector<BucketResult>> results =
      DecryptResults(ctx, sk, encrypted_blob, key_bits, record_bytes,
                     bytes_per_slot, /*buckets=*/1, /*offset=*/0, /*limit=*/1);
  if (!results.ok())
    return results.status();
  if (results->empty() || !(*results)[0].present) {
    return std::nullopt; // no row matched
  }
  return (*results)[0].record;
}

} // namespace opaquedb::crypto

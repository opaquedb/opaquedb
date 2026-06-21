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

absl::StatusOr<std::string> BuildEncryptedOperand(const CryptoContext &ctx,
                                                  const seal::PublicKey &pub,
                                                  std::uint64_t value,
                                                  std::uint32_t key_bits) {
  if (key_bits == 0 || key_bits > 64) {
    return absl::InvalidArgumentError("key_bits must be in [1, 64]");
  }
  // The query value's bits, tiled across every slot so each record's block of
  // key_bits slots compares against the same value. slot i holds bit i %
  // key_bits.
  const std::vector<std::uint64_t> bits = core::KeyToBits(value, key_bits);
  std::vector<std::uint64_t> slots(ctx.slot_count());
  for (std::size_t i = 0; i < slots.size(); ++i)
    slots[i] = bits[i % key_bits];

  absl::StatusOr<seal::Plaintext> plain = ctx.EncodeBatch(slots);
  if (!plain.ok())
    return plain.status();
  absl::StatusOr<seal::Ciphertext> cipher = Encrypt(ctx, pub, *plain);
  if (!cipher.ok())
    return cipher.status();
  return SerializeCiphertexts(
      std::vector<seal::Ciphertext>{*std::move(cipher)});
}

absl::StatusOr<std::optional<std::vector<std::uint8_t>>>
DecryptRecord(const CryptoContext &ctx, const seal::SecretKey &sk,
              const std::string &encrypted_blob, std::uint32_t key_bits,
              std::uint32_t record_bytes, std::uint32_t bytes_per_slot) {
  const std::uint32_t planes =
      core::PayloadPlaneCount(record_bytes, key_bits, bytes_per_slot);
  // planes payload ciphertexts plus one trailing presence ciphertext.
  const std::uint32_t expected = planes + 1;
  absl::StatusOr<std::vector<seal::Ciphertext>> ciphers =
      DeserializeCiphertexts(ctx, encrypted_blob, /*max_count=*/expected);
  if (!ciphers.ok())
    return ciphers.status();
  if (ciphers->size() != expected) {
    return absl::InternalError(absl::StrCat(
        "result has ", ciphers->size(), " ciphertexts, expected ", expected));
  }

  // Decodes a ciphertext's block 0, summing the two slot rows (the matched
  // payload lands in block 0 of whichever row holds it; the other is zero).
  auto block0 = [&](const seal::Ciphertext &c)
      -> absl::StatusOr<std::vector<std::uint64_t>> {
    absl::StatusOr<seal::Plaintext> plain = Decrypt(ctx, sk, c);
    if (!plain.ok())
      return plain.status();
    absl::StatusOr<std::vector<std::uint64_t>> slots = ctx.DecodeBatch(*plain);
    if (!slots.ok())
      return slots.status();
    const std::size_t row = slots->size() / 2;
    if (row < key_bits) {
      return absl::InternalError("decoded plane has fewer slots than key_bits");
    }
    std::vector<std::uint64_t> block(key_bits);
    for (std::uint32_t i = 0; i < key_bits; ++i)
      block[i] = (*slots)[i] + (*slots)[row + i];
    return block;
  };

  // Presence first: the trailing ciphertext's block-0 slot 0 is the match
  // count.
  absl::StatusOr<std::vector<std::uint64_t>> presence =
      block0((*ciphers)[planes]);
  if (!presence.ok())
    return presence.status();
  if ((*presence)[0] == 0) {
    return std::nullopt; // no row matched
  }

  const std::uint32_t bytes_per_plane =
      core::PayloadBytesPerBlock(key_bits, bytes_per_slot);
  std::vector<std::uint8_t> record;
  record.reserve(static_cast<std::size_t>(planes) * bytes_per_plane);
  for (std::uint32_t p = 0; p < planes; ++p) {
    absl::StatusOr<std::vector<std::uint64_t>> block = block0((*ciphers)[p]);
    if (!block.ok())
      return block.status();
    absl::StatusOr<std::vector<std::uint8_t>> chunk =
        core::UnpackSlotsToBytes(*block, bytes_per_plane, bytes_per_slot);
    if (!chunk.ok())
      return chunk.status();
    record.insert(record.end(), chunk->begin(), chunk->end());
  }
  record.resize(record_bytes);
  return record;
}

} // namespace opaquedb::crypto

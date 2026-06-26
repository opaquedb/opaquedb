#ifndef OPAQUEDB_CRYPTO_OPS_H_
#define OPAQUEDB_CRYPTO_OPS_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "absl/status/statusor.h"
#include "opaquedb/crypto/context.h"
#include "seal/seal.h"

// Thin status-returning wrappers over the SEAL primitives the query path uses:
// encrypt under a public key, decrypt under a secret key, and read the
// remaining noise budget. Backends build on these and on the SEAL Evaluator
// directly.

namespace opaquedb::crypto {

absl::StatusOr<seal::Ciphertext> Encrypt(const CryptoContext &ctx,
                                         const seal::PublicKey &public_key,
                                         const seal::Plaintext &plain);

absl::StatusOr<seal::Plaintext> Decrypt(const CryptoContext &ctx,
                                        const seal::SecretKey &secret_key,
                                        const seal::Ciphertext &cipher);

// The remaining invariant noise budget in bits. Zero or negative means the
// ciphertext can no longer be decrypted correctly. Tests assert this stays
// positive after evaluation. Requires the secret key, so it is a client-side
// or test-only diagnostic.
absl::StatusOr<int> NoiseBudgetBits(const CryptoContext &ctx,
                                    const seal::SecretKey &secret_key,
                                    const seal::Ciphertext &cipher);

// Helpers used by clients (and tests) to turn a plaintext lookup value into the
// encrypted operand the server expects, and to turn a result ciphertext blob
// back into a plaintext record. Centralizing here avoids duplicating the
// bit-slice + batch encode/encrypt and the decrypt+unpack dance.

// Builds the encrypted query operand: one ciphertext holding the lookup value
// binary-expanded into key_bits bits and tiled across every BatchEncoder slot,
// so each record's block compares against the same value. Returned as a
// serialized one-element ciphertext list (the wire form the server
// deserializes).
absl::StatusOr<std::string> BuildEncryptedOperand(const CryptoContext &ctx,
                                                  const seal::PublicKey &pub,
                                                  std::uint64_t value,
                                                  std::uint32_t key_bits);

// Decrypts a result record. The blob is the serialized list the matcher
// returned: one ciphertext per payload plane, then a trailing presence
// ciphertext holding the match count. If the count is zero no row matched, so
// this returns nullopt (an empty result). Otherwise each plane carries the
// matched record's bytes in slots [0, key_bits); it unpacks bytes_per_slot
// bytes each, concatenates the planes, and truncates to record_bytes.
absl::StatusOr<std::optional<std::vector<std::uint8_t>>>
DecryptRecord(const CryptoContext &ctx, const seal::SecretKey &sk,
              const std::string &encrypted_blob, std::uint32_t key_bits,
              std::uint32_t record_bytes, std::uint32_t bytes_per_slot);

// One decoded result bucket from a multi-match query.
struct BucketResult {
  bool present = false;  // a row matched in this bucket
  bool collided = false; // more than one row matched, so the bytes are garbage
  std::vector<std::uint8_t> record; // valid only when present and not collided
};

// Decrypts a multi-bucket result. The matcher partitioned matches into
// `buckets` buckets; this reads the window [offset, offset + limit) of them
// (clamped to the bucket count) and returns one BucketResult per bucket in that
// window, in bucket order. Each bucket carries its own presence count: zero
// means the bucket is empty (present = false), one means a clean match, and two
// or more means rows collided in that bucket (collided = true, bytes dropped).
// For the single-match path call with buckets = 1, offset = 0, limit = 1; that
// bucket holds the sum of every block, matching the original DecryptRecord
// behavior.
absl::StatusOr<std::vector<BucketResult>>
DecryptResults(const CryptoContext &ctx, const seal::SecretKey &sk,
               const std::string &encrypted_blob, std::uint32_t key_bits,
               std::uint32_t record_bytes, std::uint32_t bytes_per_slot,
               std::uint32_t buckets, std::uint64_t offset,
               std::uint64_t limit);

} // namespace opaquedb::crypto

#endif // OPAQUEDB_CRYPTO_OPS_H_

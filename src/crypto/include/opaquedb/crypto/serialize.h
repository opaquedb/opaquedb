#ifndef OPAQUEDB_CRYPTO_SERIALIZE_H_
#define OPAQUEDB_CRYPTO_SERIALIZE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "opaquedb/crypto/context.h"
#include "seal/seal.h"

// Wire serialization for a list of ciphertexts, the form the encrypted query
// parameter travels in. The format is a 32-bit count followed by
// length-prefixed SEAL blobs. Deserialization is the trust boundary for
// client-supplied ciphertexts: it bounds the count and every length against the
// available bytes and loads each ciphertext against the node context, which
// validates its parms_id, so a malformed or mismatched ciphertext is rejected
// with a status and never crashes the node.

namespace opaquedb::crypto {

std::string SerializeCiphertexts(const std::vector<seal::Ciphertext> &ciphers);

// Deserializes a length-framed ciphertext list, bounding the count and every
// length against the buffer and loading each ciphertext against the node
// context (which validates its parms_id). When require_top_level is true the
// ciphertexts must be fresh encryptions at the top of the modulus chain: this
// is the trust boundary for a client-supplied query operand. Server-produced
// ciphertexts (shard partials, combined results) may have been mod-switched
// down the chain during evaluation, so callers reading those pass false; load()
// still binds each to the context, and the chain level is validated.
absl::StatusOr<std::vector<seal::Ciphertext>>
DeserializeCiphertexts(const CryptoContext &ctx, const std::string &bytes,
                       std::uint32_t max_count, bool require_top_level = true);

} // namespace opaquedb::crypto

#endif // OPAQUEDB_CRYPTO_SERIALIZE_H_

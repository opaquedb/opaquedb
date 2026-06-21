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

absl::StatusOr<std::vector<seal::Ciphertext>>
DeserializeCiphertexts(const CryptoContext &ctx, const std::string &bytes,
                       std::uint32_t max_count);

} // namespace opaquedb::crypto

#endif // OPAQUEDB_CRYPTO_SERIALIZE_H_

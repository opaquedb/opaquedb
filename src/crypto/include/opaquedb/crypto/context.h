#ifndef OPAQUEDB_CRYPTO_CONTEXT_H_
#define OPAQUEDB_CRYPTO_CONTEXT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/config/config.h"
#include "seal/seal.h"

// The crypto layer is the SEAL boundary. It reads its parameters from config;
// it does not define them. crypto knows nothing about SQL, storage, or
// transport.
//
// SEAL types appear in these public headers on purpose. crypto is the adapter
// that owns SEAL, and the backend interface (a higher layer) already speaks in
// seal::Ciphertext. Hiding SEAL behind a Pimpl here would mean re-wrapping
// every key and ciphertext type for no caller that benefits, which is the kind
// of abstraction the project asks us not to add. Pimpl is reserved for headers
// that pull SEAL only incidentally.

namespace opaquedb::crypto {

// A built BFV context plus its BatchEncoder, the shared SEAL state every other
// crypto operation needs. Cheap to copy; it holds shared pointers.
class CryptoContext {
public:
  // Builds the context from the FHE parameter set in config. Validates that the
  // parameters admit batching and a valid coefficient modulus, converting
  // SEAL's exceptions into a status so an operator's bad config never crashes a
  // node.
  static absl::StatusOr<CryptoContext> Create(const config::CryptoConfig &cfg);

  const seal::SEALContext &seal() const { return *context_; }
  std::shared_ptr<seal::SEALContext> seal_ptr() const { return context_; }

  // Number of BatchEncoder slots, equal to poly_modulus_degree.
  std::size_t slot_count() const { return encoder_->slot_count(); }

  // The plaintext modulus, the field the slot values live in.
  std::uint64_t plain_modulus() const;

  // Encodes slot values into a plaintext. Values longer than slot_count are
  // rejected; shorter inputs are zero-padded by the encoder.
  absl::StatusOr<seal::Plaintext>
  EncodeBatch(const std::vector<std::uint64_t> &values) const;

  // Decodes a plaintext back into slot_count slot values.
  absl::StatusOr<std::vector<std::uint64_t>>
  DecodeBatch(const seal::Plaintext &plain) const;

private:
  CryptoContext(std::shared_ptr<seal::SEALContext> context,
                std::shared_ptr<seal::BatchEncoder> encoder)
      : context_(std::move(context)), encoder_(std::move(encoder)) {}

  std::shared_ptr<seal::SEALContext> context_;
  std::shared_ptr<seal::BatchEncoder> encoder_;
};

} // namespace opaquedb::crypto

#endif // OPAQUEDB_CRYPTO_CONTEXT_H_

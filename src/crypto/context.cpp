#include "opaquedb/crypto/context.h"

#include <cstddef>
#include <exception>
#include <vector>

#include "absl/strings/str_cat.h"

namespace opaquedb::crypto {

absl::StatusOr<CryptoContext>
CryptoContext::Create(const config::CryptoConfig &cfg) {
  // config::Validate has already checked the shape of these values (power of
  // two, sane bit sizes). Here SEAL does the deeper feasibility checks, and any
  // failure surfaces as a status rather than an exception out of a node.
  try {
    seal::EncryptionParameters params(seal::scheme_type::bfv);
    params.set_poly_modulus_degree(cfg.poly_modulus_degree);

    std::vector<int> bit_sizes(cfg.coeff_modulus_bits.begin(),
                               cfg.coeff_modulus_bits.end());
    params.set_coeff_modulus(
        seal::CoeffModulus::Create(cfg.poly_modulus_degree, bit_sizes));

    // PlainModulus::Batching finds a prime congruent to 1 mod 2n so the
    // BatchEncoder has full SIMD slots. Without it constant-weight bits could
    // not be packed into slots.
    params.set_plain_modulus(seal::PlainModulus::Batching(
        cfg.poly_modulus_degree, static_cast<int>(cfg.plain_modulus_bits)));

    auto context = std::make_shared<seal::SEALContext>(params);
    if (!context->parameters_set()) {
      return absl::InvalidArgumentError(
          absl::StrCat("FHE parameters rejected by SEAL: ",
                       context->parameter_error_message()));
    }
    auto encoder = std::make_shared<seal::BatchEncoder>(*context);
    return CryptoContext(std::move(context), std::move(encoder));
  } catch (const std::exception &e) {
    return absl::InvalidArgumentError(
        absl::StrCat("failed to build FHE context: ", e.what()));
  }
}

std::uint64_t CryptoContext::plain_modulus() const {
  return context_->first_context_data()->parms().plain_modulus().value();
}

absl::StatusOr<seal::Plaintext>
CryptoContext::EncodeBatch(const std::vector<std::uint64_t> &values) const {
  if (values.size() > slot_count()) {
    return absl::InvalidArgumentError(absl::StrCat("batch of ", values.size(),
                                                   " values exceeds ",
                                                   slot_count(), " slots"));
  }
  try {
    seal::Plaintext plain;
    encoder_->encode(values, plain);
    return plain;
  } catch (const std::exception &e) {
    return absl::InternalError(absl::StrCat("batch encode failed: ", e.what()));
  }
}

absl::StatusOr<std::vector<std::uint64_t>>
CryptoContext::DecodeBatch(const seal::Plaintext &plain) const {
  try {
    std::vector<std::uint64_t> values;
    encoder_->decode(plain, values);
    return values;
  } catch (const std::exception &e) {
    return absl::InternalError(absl::StrCat("batch decode failed: ", e.what()));
  }
}

} // namespace opaquedb::crypto

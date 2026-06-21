#include "opaquedb/crypto/key_material.h"

#include <exception>
#include <sstream>
#include <string>

#include "absl/strings/str_cat.h"

namespace opaquedb::crypto {
namespace {

// Serializes a SEAL Serializable/saveable object to a string.
template <typename T> std::string SaveToString(const T &object) {
  std::ostringstream stream(std::ios::binary);
  object.save(stream);
  return stream.str();
}

// Loads a SEAL key from bytes, binding and validating it against the context.
template <typename T>
absl::StatusOr<T> LoadFromString(const seal::SEALContext &context,
                                 const std::string &bytes, const char *what) {
  try {
    T object;
    std::istringstream stream(bytes, std::ios::binary);
    object.load(context, stream);
    return object;
  } catch (const std::exception &e) {
    return absl::InvalidArgumentError(
        absl::StrCat("rejected ", what, ": ", e.what()));
  }
}

} // namespace

ClientKeyring ClientKeyring::Generate(const CryptoContext &ctx,
                                      bool with_galois) {
  seal::KeyGenerator keygen(ctx.seal());
  ClientKeyring keyring;
  keyring.secret_key_ = keygen.secret_key();
  keygen.create_public_key(keyring.public_key_);
  keygen.create_relin_keys(keyring.relin_keys_);
  // Galois keys (the power-of-two rotation steps and column swap) drive the
  // matcher's AND reduction, masked broadcast, and block-sum retrieve. They are
  // the largest key set but the algorithm needs them, so they are on by
  // default.
  if (with_galois) {
    keygen.create_galois_keys(keyring.galois_keys_);
    keyring.has_galois_ = true;
  }
  return keyring;
}

ClientKeyring ClientKeyring::Generate(const CryptoContext &ctx,
                                      const std::vector<int> &galois_steps) {
  seal::KeyGenerator keygen(ctx.seal());
  ClientKeyring keyring;
  keyring.secret_key_ = keygen.secret_key();
  keygen.create_public_key(keyring.public_key_);
  keygen.create_relin_keys(keyring.relin_keys_);
  if (!galois_steps.empty()) {
    keygen.create_galois_keys(galois_steps, keyring.galois_keys_);
    keyring.has_galois_ = true;
  }
  return keyring;
}

absl::StatusOr<KeyMaterial> ClientKeyring::SerializePublic() const {
  try {
    KeyMaterial material;
    material.public_key = SaveToString(public_key_);
    material.relin_keys = SaveToString(relin_keys_);
    // Empty when no Galois keys were generated, so nothing large crosses the
    // wire and the node skips loading them.
    material.galois_keys = has_galois_ ? SaveToString(galois_keys_) : "";
    return material;
  } catch (const std::exception &e) {
    return absl::InternalError(
        absl::StrCat("failed to serialize keys: ", e.what()));
  }
}

absl::StatusOr<EvalKeys> LoadKeyMaterial(const CryptoContext &ctx,
                                         const KeyMaterial &material) {
  EvalKeys keys;
  absl::StatusOr<seal::PublicKey> pk = LoadFromString<seal::PublicKey>(
      ctx.seal(), material.public_key, "public key");
  if (!pk.ok())
    return pk.status();
  keys.public_key = *std::move(pk);

  absl::StatusOr<seal::RelinKeys> rk = LoadFromString<seal::RelinKeys>(
      ctx.seal(), material.relin_keys, "relinearization keys");
  if (!rk.ok())
    return rk.status();
  keys.relin_keys = *std::move(rk);

  // Galois keys are optional: an empty field means the client generated none
  // (the reference backend uses no rotations), so there is nothing to load.
  if (!material.galois_keys.empty()) {
    absl::StatusOr<seal::GaloisKeys> gk = LoadFromString<seal::GaloisKeys>(
        ctx.seal(), material.galois_keys, "Galois keys");
    if (!gk.ok())
      return gk.status();
    keys.galois_keys = *std::move(gk);
  }

  return keys;
}

} // namespace opaquedb::crypto

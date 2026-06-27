#include "opaquedb/crypto/key_material.h"

#include <exception>
#include <sstream>
#include <string>

#include "absl/strings/str_cat.h"

namespace opaquedb::crypto {
namespace {

// Serializes a SEAL Serializable/saveable object to a string. For the
// Serializable<T> wrappers returned by KeyGenerator::create_*, save() emits the
// seeded form: the second polynomial of every key component is replaced by the
// 32-byte PRNG seed it was generated from, roughly halving the bytes. The big
// Galois set is the main beneficiary.
template <typename T> std::string SaveToString(const T &object) {
  std::ostringstream stream(std::ios::binary);
  object.save(stream);
  return stream.str();
}

// Materializes a seeded key blob back into a full SEAL key, binding it to the
// context. Used to keep the local accessors working after we only kept the
// seeded bytes. The bytes are ones we just produced, so a failure here is a
// programmer error, not untrusted input; load() throws and the caller treats
// that as fatal.
template <typename T>
T LoadObject(const seal::SEALContext &context, const std::string &bytes) {
  T object;
  std::istringstream stream(bytes, std::ios::binary);
  object.load(context, stream);
  return object;
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
  // Generate the keys directly into their seeded wire form (see SaveToString).
  // The local materialized copies the accessors hand out are loaded back from
  // those bytes, so each key is generated exactly once.
  keyring.public_key_bytes_ = SaveToString(keygen.create_public_key());
  keyring.relin_keys_bytes_ = SaveToString(keygen.create_relin_keys());
  // Galois keys (the power-of-two rotation steps and column swap) drive the
  // matcher's AND reduction, masked broadcast, and block-sum retrieve. They are
  // the largest key set but the algorithm needs them, so they are on by
  // default.
  if (with_galois) {
    keyring.galois_keys_bytes_ = SaveToString(keygen.create_galois_keys());
    keyring.has_galois_ = true;
  }
  keyring.Materialize(ctx.seal());
  return keyring;
}

ClientKeyring ClientKeyring::Generate(const CryptoContext &ctx,
                                      const std::vector<int> &galois_steps) {
  seal::KeyGenerator keygen(ctx.seal());
  ClientKeyring keyring;
  keyring.secret_key_ = keygen.secret_key();
  keyring.public_key_bytes_ = SaveToString(keygen.create_public_key());
  keyring.relin_keys_bytes_ = SaveToString(keygen.create_relin_keys());
  if (!galois_steps.empty()) {
    keyring.galois_keys_bytes_ =
        SaveToString(keygen.create_galois_keys(galois_steps));
    keyring.has_galois_ = true;
  }
  keyring.Materialize(ctx.seal());
  return keyring;
}

void ClientKeyring::Materialize(const seal::SEALContext &context) {
  public_key_ = LoadObject<seal::PublicKey>(context, public_key_bytes_);
  relin_keys_ = LoadObject<seal::RelinKeys>(context, relin_keys_bytes_);
  if (has_galois_)
    galois_keys_ = LoadObject<seal::GaloisKeys>(context, galois_keys_bytes_);
}

absl::StatusOr<KeyMaterial> ClientKeyring::SerializePublic() const {
  // The seeded bytes were produced at generation time; just hand them over.
  // Empty galois_keys means none were generated, so nothing large crosses the
  // wire and the node skips loading them.
  KeyMaterial material;
  material.public_key = public_key_bytes_;
  material.relin_keys = relin_keys_bytes_;
  material.galois_keys = has_galois_ ? galois_keys_bytes_ : "";
  return material;
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

#include "opaquedb/crypto/key_material.h"

#include <cstdint>
#include <exception>
#include <sstream>
#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"

namespace opaquedb::crypto {
namespace {

// Magic and version for the local full-keyset blob (SerializeAll/LoadAll). A
// mismatch means the bytes are not ours or are from an incompatible layout.
constexpr std::string_view kKeyringMagic = "OQCKR01";

void AppendU64(std::string &out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>(value & 0xFF));
    value >>= 8;
  }
}

// Reads a length-prefixed blob, validating the declared length against the
// bytes that actually remain, so a truncated or hostile blob cannot over-read.
bool ReadBlob(std::string_view buf, std::size_t &offset, std::string &out) {
  if (offset + 8 > buf.size())
    return false;
  std::uint64_t len = 0;
  for (std::size_t i = 0; i < 8; ++i)
    len |=
        static_cast<std::uint64_t>(static_cast<unsigned char>(buf[offset + i]))
        << (8 * i);
  offset += 8;
  if (len > buf.size() - offset)
    return false;
  out.assign(buf.data() + offset, len);
  offset += len;
  return true;
}

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

absl::StatusOr<std::string> ClientKeyring::SerializeAll() const {
  std::string secret_bytes;
  try {
    secret_bytes = SaveToString(secret_key_);
  } catch (const std::exception &e) {
    return absl::InternalError(
        absl::StrCat("failed to serialize secret key: ", e.what()));
  }
  std::string out;
  out.append(kKeyringMagic);
  out.push_back(has_galois_ ? 1 : 0);
  AppendU64(out, secret_bytes.size());
  out.append(secret_bytes);
  AppendU64(out, public_key_bytes_.size());
  out.append(public_key_bytes_);
  AppendU64(out, relin_keys_bytes_.size());
  out.append(relin_keys_bytes_);
  AppendU64(out, galois_keys_bytes_.size());
  out.append(galois_keys_bytes_);
  return out;
}

absl::StatusOr<ClientKeyring> ClientKeyring::LoadAll(const CryptoContext &ctx,
                                                     std::string_view bytes) {
  if (bytes.size() < kKeyringMagic.size() + 1 ||
      bytes.substr(0, kKeyringMagic.size()) != kKeyringMagic)
    return absl::InvalidArgumentError("keyset blob has a bad or missing magic");
  std::size_t offset = kKeyringMagic.size();
  const bool has_galois = bytes[offset++] != 0;

  std::string secret_bytes;
  ClientKeyring keyring;
  if (!ReadBlob(bytes, offset, secret_bytes) ||
      !ReadBlob(bytes, offset, keyring.public_key_bytes_) ||
      !ReadBlob(bytes, offset, keyring.relin_keys_bytes_) ||
      !ReadBlob(bytes, offset, keyring.galois_keys_bytes_))
    return absl::InvalidArgumentError("keyset blob is truncated or malformed");
  keyring.has_galois_ = has_galois;

  // Bind every key to the context, validating each against its parameters. A
  // mismatched or corrupted blob is rejected with a status, never a crash.
  absl::StatusOr<seal::SecretKey> sk =
      LoadFromString<seal::SecretKey>(ctx.seal(), secret_bytes, "secret key");
  if (!sk.ok())
    return sk.status();
  keyring.secret_key_ = *std::move(sk);

  absl::StatusOr<seal::PublicKey> pk = LoadFromString<seal::PublicKey>(
      ctx.seal(), keyring.public_key_bytes_, "public key");
  if (!pk.ok())
    return pk.status();
  keyring.public_key_ = *std::move(pk);

  absl::StatusOr<seal::RelinKeys> rk = LoadFromString<seal::RelinKeys>(
      ctx.seal(), keyring.relin_keys_bytes_, "relinearization keys");
  if (!rk.ok())
    return rk.status();
  keyring.relin_keys_ = *std::move(rk);

  if (has_galois) {
    absl::StatusOr<seal::GaloisKeys> gk = LoadFromString<seal::GaloisKeys>(
        ctx.seal(), keyring.galois_keys_bytes_, "Galois keys");
    if (!gk.ok())
      return gk.status();
    keyring.galois_keys_ = *std::move(gk);
  }
  return keyring;
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

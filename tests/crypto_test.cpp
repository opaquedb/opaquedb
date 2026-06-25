#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "opaquedb/config/config.h"
#include "opaquedb/crypto/context.h"
#include "opaquedb/crypto/key_material.h"
#include "opaquedb/crypto/ops.h"

namespace {

using opaquedb::config::CryptoConfig;
using opaquedb::crypto::ClientKeyring;
using opaquedb::crypto::CryptoContext;
using opaquedb::crypto::Decrypt;
using opaquedb::crypto::Encrypt;
using opaquedb::crypto::EvalKeys;
using opaquedb::crypto::KeyMaterial;
using opaquedb::crypto::LoadKeyMaterial;
using opaquedb::crypto::NoiseBudgetBits;

CryptoContext MakeContext() {
  CryptoConfig
      cfg; // the documented defaults: poly 16384, 20-bit plain modulus.
  auto ctx = CryptoContext::Create(cfg);
  EXPECT_TRUE(ctx.ok()) << ctx.status().message();
  return *ctx;
}

TEST(Crypto, BuildsContextWithDefaultParams) {
  CryptoContext ctx = MakeContext();
  EXPECT_EQ(ctx.slot_count(), 16384u);
  EXPECT_GT(ctx.plain_modulus(), 0u);
}

TEST(Crypto, RejectsNonPowerOfTwoDegree) {
  CryptoConfig cfg;
  cfg.poly_modulus_degree = 8000; // not a power of two
  EXPECT_FALSE(CryptoContext::Create(cfg).ok());
}

TEST(Crypto, BatchEncodeDecodeRoundTrips) {
  CryptoContext ctx = MakeContext();
  std::vector<std::uint64_t> values(ctx.slot_count(), 0);
  for (std::size_t i = 0; i < 32; ++i)
    values[i] = i % 7;

  auto plain = ctx.EncodeBatch(values);
  ASSERT_TRUE(plain.ok()) << plain.status().message();
  auto decoded = ctx.DecodeBatch(*plain);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  EXPECT_EQ(*decoded, values);
}

TEST(Crypto, EncryptDecryptRoundTripsWithPositiveNoiseBudget) {
  CryptoContext ctx = MakeContext();
  ClientKeyring keyring = ClientKeyring::Generate(ctx);

  std::vector<std::uint64_t> values(ctx.slot_count(), 0);
  values[0] = 42;
  values[1] = 7;

  auto plain = ctx.EncodeBatch(values);
  ASSERT_TRUE(plain.ok());
  auto cipher = Encrypt(ctx, keyring.public_key(), *plain);
  ASSERT_TRUE(cipher.ok()) << cipher.status().message();

  auto budget = NoiseBudgetBits(ctx, keyring.secret_key(), *cipher);
  ASSERT_TRUE(budget.ok());
  EXPECT_GT(*budget, 0) << "fresh ciphertext must be decryptable";

  auto back = Decrypt(ctx, keyring.secret_key(), *cipher);
  ASSERT_TRUE(back.ok());
  auto decoded = ctx.DecodeBatch(*back);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ((*decoded)[0], 42u);
  EXPECT_EQ((*decoded)[1], 7u);
}

TEST(Crypto, KeyMaterialSerializesAndReloads) {
  CryptoContext ctx = MakeContext();
  // The default keyring carries Galois keys, which the bit-sliced matcher
  // needs.
  ClientKeyring keyring = ClientKeyring::Generate(ctx);

  auto material = keyring.SerializePublic();
  ASSERT_TRUE(material.ok()) << material.status().message();
  EXPECT_FALSE(material->public_key.empty());
  EXPECT_FALSE(material->relin_keys.empty());
  EXPECT_FALSE(material->galois_keys.empty());

  auto keys = LoadKeyMaterial(ctx, *material);
  ASSERT_TRUE(keys.ok()) << keys.status().message();

  // A backend that needs no rotations can opt out; then the field is empty and
  // loading still succeeds.
  ClientKeyring no_galois = ClientKeyring::Generate(ctx, /*with_galois=*/false);
  auto nmaterial = no_galois.SerializePublic();
  ASSERT_TRUE(nmaterial.ok()) << nmaterial.status().message();
  EXPECT_TRUE(nmaterial->galois_keys.empty());
  EXPECT_TRUE(LoadKeyMaterial(ctx, *nmaterial).ok());

  // The reloaded public key must encrypt to something the client can decrypt.
  std::vector<std::uint64_t> values(ctx.slot_count(), 0);
  values[0] = 99;
  auto plain = ctx.EncodeBatch(values);
  ASSERT_TRUE(plain.ok());
  auto cipher = Encrypt(ctx, keys->public_key, *plain);
  ASSERT_TRUE(cipher.ok());
  auto back = Decrypt(ctx, keyring.secret_key(), *cipher);
  ASSERT_TRUE(back.ok());
  auto decoded = ctx.DecodeBatch(*back);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ((*decoded)[0], 99u);
}

TEST(Crypto, LoadRejectsGarbageKeyMaterial) {
  CryptoContext ctx = MakeContext();
  KeyMaterial garbage;
  garbage.public_key = "not a key";
  garbage.relin_keys = "nope";
  garbage.galois_keys = "still no";
  EXPECT_FALSE(LoadKeyMaterial(ctx, garbage).ok());
}

} // namespace

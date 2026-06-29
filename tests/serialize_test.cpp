#include "opaquedb/crypto/serialize.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "opaquedb/config/config.h"
#include "opaquedb/crypto/context.h"
#include "opaquedb/crypto/key_material.h"
#include "opaquedb/crypto/ops.h"

// DeserializeCiphertexts is the trust boundary for every client-supplied
// ciphertext: a query operand arrives as untrusted bytes and must be rejected
// with a status, never crash the node, when it is truncated, oversized, framed
// past the buffer, garbage, or built for different FHE parameters. These tests
// drive that boundary directly with crafted bytes. They are cheap (no matcher,
// no rotations) so they belong in the fast tier.

namespace {

using opaquedb::config::CryptoConfig;
using opaquedb::crypto::ClientKeyring;
using opaquedb::crypto::CryptoContext;
using opaquedb::crypto::DeserializeCiphertexts;
using opaquedb::crypto::Encrypt;
using opaquedb::crypto::SerializeCiphertexts;

CryptoContext MakeContext(std::uint32_t plain_modulus_bits = 20) {
  CryptoConfig cfg; // documented defaults: poly 16384, 349-bit coeff modulus
  cfg.plain_modulus_bits = plain_modulus_bits;
  auto ctx = CryptoContext::Create(cfg);
  EXPECT_TRUE(ctx.ok()) << ctx.status().message();
  return *ctx;
}

// A real, well-formed one-ciphertext blob the negative cases can corrupt.
std::string ValidBlob(const CryptoContext &ctx, const ClientKeyring &keyring) {
  std::vector<std::uint64_t> values(ctx.slot_count(), 0);
  values[0] = 1;
  auto plain = ctx.EncodeBatch(values);
  EXPECT_TRUE(plain.ok());
  auto cipher = Encrypt(ctx, keyring.public_key(), *plain);
  EXPECT_TRUE(cipher.ok()) << cipher.status().message();
  return SerializeCiphertexts({*cipher});
}

TEST(DeserializeCiphertexts, AcceptsAWellFormedClientOperand) {
  CryptoContext ctx = MakeContext();
  ClientKeyring keyring = ClientKeyring::Generate(ctx, /*with_galois=*/false);
  std::string blob = ValidBlob(ctx, keyring);

  auto got = DeserializeCiphertexts(ctx, blob, /*max_count=*/1);
  ASSERT_TRUE(got.ok()) << got.status().message();
  EXPECT_EQ(got->size(), 1u);
}

TEST(DeserializeCiphertexts, RejectsTruncatedCountHeader) {
  CryptoContext ctx = MakeContext();
  // Fewer than the four bytes a count needs.
  EXPECT_FALSE(DeserializeCiphertexts(ctx, std::string("\x01\x00", 2), 4).ok());
  EXPECT_FALSE(DeserializeCiphertexts(ctx, std::string(), 4).ok());
}

TEST(DeserializeCiphertexts, RejectsCountOverTheLimit) {
  CryptoContext ctx = MakeContext();
  // A count of 9 declared, but the caller allows at most 4.
  std::string bytes("\x09\x00\x00\x00", 4);
  auto got = DeserializeCiphertexts(ctx, bytes, /*max_count=*/4);
  EXPECT_FALSE(got.ok());
}

TEST(DeserializeCiphertexts, RejectsADeclaredLengthThatRunsPastTheBuffer) {
  CryptoContext ctx = MakeContext();
  // count=1, then a length of 0xFFFFFFFF with no payload following it.
  std::string bytes;
  bytes.append("\x01\x00\x00\x00", 4);
  bytes.append("\xFF\xFF\xFF\xFF", 4);
  auto got = DeserializeCiphertexts(ctx, bytes, /*max_count=*/4);
  EXPECT_FALSE(got.ok());
}

TEST(DeserializeCiphertexts, RejectsTruncatedPerItemLength) {
  CryptoContext ctx = MakeContext();
  // count=1 but only two of the length's four bytes are present.
  std::string bytes;
  bytes.append("\x01\x00\x00\x00", 4);
  bytes.append("\x10\x00", 2);
  EXPECT_FALSE(DeserializeCiphertexts(ctx, bytes, /*max_count=*/4).ok());
}

TEST(DeserializeCiphertexts, RejectsGarbagePayload) {
  CryptoContext ctx = MakeContext();
  // count=1, length=8, eight bytes that are not a SEAL ciphertext.
  std::string bytes;
  bytes.append("\x01\x00\x00\x00", 4);
  bytes.append("\x08\x00\x00\x00", 4);
  bytes.append("garbage!", 8);
  auto got = DeserializeCiphertexts(ctx, bytes, /*max_count=*/4);
  EXPECT_FALSE(got.ok()) << "a non-SEAL blob must be rejected, not crash";
}

TEST(DeserializeCiphertexts, RejectsTrailingBytesAfterTheList) {
  CryptoContext ctx = MakeContext();
  ClientKeyring keyring = ClientKeyring::Generate(ctx, /*with_galois=*/false);
  std::string blob = ValidBlob(ctx, keyring);
  blob.append("extra", 5); // unconsumed bytes after a valid ciphertext
  auto got = DeserializeCiphertexts(ctx, blob, /*max_count=*/1);
  EXPECT_FALSE(got.ok()) << "trailing bytes signal a malformed or padded blob";
}

TEST(BuildEncryptedOperands, DedupsRepeatedValues) {
  // WHERE k IN (5, 5) must not build two operands: the matcher sums per-operand
  // indicators assuming they are disjoint, so a duplicate would double a
  // matching row's indicator and silently drop it. Dedup happens here, the one
  // point every operand list flows through.
  CryptoContext ctx = MakeContext();
  ClientKeyring keyring = ClientKeyring::Generate(ctx, /*with_galois=*/false);

  auto one = opaquedb::crypto::BuildEncryptedOperands(ctx, keyring.public_key(),
                                                      {5, 5}, /*key_bits=*/16);
  ASSERT_TRUE(one.ok()) << one.status().message();
  auto cts = DeserializeCiphertexts(ctx, *one, /*max_count=*/2);
  ASSERT_TRUE(cts.ok()) << cts.status().message();
  EXPECT_EQ(cts->size(), 1u) << "the repeated value 5 collapses to one operand";

  // A genuine multi-value list keeps each distinct value.
  auto three = opaquedb::crypto::BuildEncryptedOperands(
      ctx, keyring.public_key(), {5, 9, 5, 9, 12}, 16);
  ASSERT_TRUE(three.ok());
  auto tcts = DeserializeCiphertexts(ctx, *three, /*max_count=*/5);
  ASSERT_TRUE(tcts.ok());
  EXPECT_EQ(tcts->size(), 3u) << "distinct values 5, 9, 12 survive";
}

TEST(DeserializeCiphertexts, RejectsCiphertextBuiltForDifferentParameters) {
  // A ciphertext encrypted under a different plain modulus carries a different
  // parms_id. Loading it against this node's context must be rejected, so a
  // client cannot smuggle in an operand from foreign FHE parameters.
  CryptoContext node = MakeContext(/*plain_modulus_bits=*/20);
  CryptoContext foreign = MakeContext(/*plain_modulus_bits=*/22);
  ClientKeyring foreign_keys =
      ClientKeyring::Generate(foreign, /*with_galois=*/false);
  std::string blob = ValidBlob(foreign, foreign_keys);

  auto got = DeserializeCiphertexts(node, blob, /*max_count=*/1);
  EXPECT_FALSE(got.ok()) << "foreign-parameter ciphertext must be rejected";
}

// SerializeAll/LoadAll is the client-side keyset persistence that lets a client
// reuse its identity and register only once. The blob carries the secret key,
// so it never crosses the wire, but it must round-trip exactly.

TEST(SerializeAll, RoundTripsTheFullKeysetIncludingSecret) {
  CryptoContext ctx = MakeContext();
  // A small Galois step set exercises the has_galois path without the cost of
  // the full power-of-two set.
  ClientKeyring keyring = ClientKeyring::Generate(ctx, std::vector<int>{1});

  auto blob = keyring.SerializeAll();
  ASSERT_TRUE(blob.ok()) << blob.status().message();

  auto loaded = ClientKeyring::LoadAll(ctx, *blob);
  ASSERT_TRUE(loaded.ok()) << loaded.status().message();

  // Re-serializing the reconstructed keyset yields identical bytes: every key,
  // secret included, survived the round trip.
  auto reblob = loaded->SerializeAll();
  ASSERT_TRUE(reblob.ok());
  EXPECT_EQ(*reblob, *blob);

  // The public wire form also matches.
  auto orig_pub = keyring.SerializePublic();
  auto load_pub = loaded->SerializePublic();
  ASSERT_TRUE(orig_pub.ok());
  ASSERT_TRUE(load_pub.ok());
  EXPECT_EQ(orig_pub->public_key, load_pub->public_key);
  EXPECT_EQ(orig_pub->relin_keys, load_pub->relin_keys);
  EXPECT_EQ(orig_pub->galois_keys, load_pub->galois_keys);
}

TEST(SerializeAll, RoundTripsWithoutGalois) {
  CryptoContext ctx = MakeContext();
  ClientKeyring keyring = ClientKeyring::Generate(ctx, /*with_galois=*/false);
  auto blob = keyring.SerializeAll();
  ASSERT_TRUE(blob.ok());
  auto loaded = ClientKeyring::LoadAll(ctx, *blob);
  ASSERT_TRUE(loaded.ok()) << loaded.status().message();
  auto pub = loaded->SerializePublic();
  ASSERT_TRUE(pub.ok());
  EXPECT_TRUE(pub->galois_keys.empty());
}

TEST(SerializeAll, RejectsGarbageAndForeignParameters) {
  CryptoContext ctx = MakeContext();
  EXPECT_FALSE(ClientKeyring::LoadAll(ctx, "").ok());
  EXPECT_FALSE(ClientKeyring::LoadAll(ctx, "not a keyset blob").ok());

  // A blob from one parameter set must not load under another.
  CryptoContext foreign = MakeContext(/*plain_modulus_bits=*/22);
  ClientKeyring fk = ClientKeyring::Generate(foreign, /*with_galois=*/false);
  auto fblob = fk.SerializeAll();
  ASSERT_TRUE(fblob.ok());
  EXPECT_FALSE(ClientKeyring::LoadAll(ctx, *fblob).ok());
}

} // namespace

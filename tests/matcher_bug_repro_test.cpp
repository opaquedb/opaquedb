#include "opaquedb/backend/reference/reference.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "opaquedb/backend/pir_backend.h"
#include "opaquedb/backend/registry.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/slot_codec.h"
#include "opaquedb/crypto/context.h"
#include "opaquedb/crypto/ops.h"
#include "opaquedb/crypto/serialize.h"

// Reproductions for two suspected correctness bugs surfaced by an adversarial
// read of the matcher. Each test asserts the CORRECT, expected behaviour, so a
// failure here is the bug, not a flaky test. They run the real matcher (SEAL),
// so they are in the slow tier.

namespace {

using opaquedb::backend::BackendRegistry;
using opaquedb::backend::EncryptedQuery;
using opaquedb::backend::KeyColumn;
using opaquedb::backend::Op;
using opaquedb::backend::PayloadColumns;
using opaquedb::backend::PirBackend;
using opaquedb::config::CryptoConfig;
using opaquedb::crypto::ClientKeyring;
using opaquedb::crypto::CryptoContext;

constexpr std::uint32_t kKeyBits = 16;
constexpr std::uint32_t kBytesPerSlot = 2;
constexpr std::uint32_t kRecordBytes = 16;
constexpr std::uint32_t kBuckets = 4;

std::vector<std::uint8_t> Payload(std::uint8_t tag) {
  std::vector<std::uint8_t> p(kRecordBytes, 0);
  p[0] = tag;
  p[1] = static_cast<std::uint8_t>(tag + 100);
  return p;
}

struct Fixture {
  CryptoContext ctx;
  ClientKeyring keyring;
  std::unique_ptr<PirBackend> backend;

  static Fixture Make() {
    CryptoConfig cfg;
    auto ctx = CryptoContext::Create(cfg);
    EXPECT_TRUE(ctx.ok()) << ctx.status().message();
    auto steps = opaquedb::core::RequiredGaloisSteps(
        static_cast<std::uint32_t>(ctx->slot_count()), kKeyBits);
    Fixture f{*ctx, ClientKeyring::Generate(*ctx, steps), nullptr};
    EXPECT_TRUE(opaquedb::backend::reference::LinkReferenceBackend());
    auto be = BackendRegistry::instance().create("reference", f.ctx);
    EXPECT_TRUE(be.ok()) << be.status().message();
    f.backend = *std::move(be);
    auto material = f.keyring.SerializePublic();
    EXPECT_TRUE(material.ok());
    EXPECT_TRUE(f.backend->load_keys(*material).ok());
    return f;
  }

  EncryptedQuery Query(std::uint64_t value) { return QueryIn({value}); }

  // Builds the query the way the client does: the whole operand list goes
  // through BuildEncryptedOperands (which dedups), then is deserialized into a
  // primary operand plus extras. This exercises the real IN/OR path, including
  // the duplicate-operand dedup fix.
  EncryptedQuery QueryIn(const std::vector<std::uint64_t> &values) {
    auto blob = opaquedb::crypto::BuildEncryptedOperands(
        ctx, keyring.public_key(), values, kKeyBits);
    EXPECT_TRUE(blob.ok()) << blob.status().message();
    auto cts = opaquedb::crypto::DeserializeCiphertexts(
        ctx, *blob, static_cast<std::uint32_t>(values.size()));
    EXPECT_TRUE(cts.ok()) << cts.status().message();
    EXPECT_FALSE(cts->empty());
    EncryptedQuery q;
    q.op = Op::kEq;
    q.query = (*cts)[0];
    q.result_buckets = kBuckets;
    for (std::size_t i = 1; i < cts->size(); ++i)
      q.extra_operands.push_back((*cts)[i]);
    return q;
  }

  KeyColumn Keys(const std::vector<std::uint64_t> &ks) {
    KeyColumn c;
    c.key_bits = kKeyBits;
    c.keys = ks;
    return c;
  }
  PayloadColumns Payloads(const std::vector<std::uint8_t> &tags) {
    PayloadColumns p;
    p.record_bytes = kRecordBytes;
    p.bytes_per_slot = kBytesPerSlot;
    for (std::uint8_t t : tags)
      p.rows.push_back(Payload(t));
    return p;
  }

  // Decode a combined result: returns (clean rows, collided count).
  std::pair<std::vector<std::vector<std::uint8_t>>, std::uint32_t>
  Decode(const std::vector<seal::Ciphertext> &planes) {
    std::string blob = opaquedb::crypto::SerializeCiphertexts(planes);
    auto recs = opaquedb::crypto::DecryptResults(
        ctx, keyring.secret_key(), blob, kKeyBits, kRecordBytes, kBytesPerSlot,
        kBuckets, 0, kBuckets);
    EXPECT_TRUE(recs.ok()) << recs.status().message();
    std::vector<std::vector<std::uint8_t>> rows;
    std::uint32_t collided = 0;
    for (const auto &br : *recs) {
      if (br.collided)
        ++collided;
      else if (br.present)
        rows.push_back(br.record);
    }
    return {rows, collided};
  }
};

// CONFIRMED BUG (distributed, secondary index): two rows with the SAME matched
// value live on two different shards (different primary keys -> different
// shards). Each shard places its single match at sequence i=0, so both land in
// bucket Mix(value); CombinePartials sums them plane-wise into one bucket, so
// both rows are lost (decoded as one collided bucket: 0 clean rows). This
// contradicts the CLAUDE.md claim that a secondary-index query "combines
// exactly like a key query ... the sum never double-counts": that holds only
// when bucket placement is keyed by the primary key (each row on one shard),
// not by a shared secondary-index value. DISABLED because the fix is a design
// change (place buckets by the primary key, or verify rows client-side), not a
// local patch. Re-enable when fixed. Verified to fail: got 0 rows, 1 collided.
TEST(MatcherBugRepro, DISABLED_CrossShardSameValueRowsAreNotLost) {
  Fixture fx = Fixture::Make();
  auto eval = fx.backend->load_keys(*fx.keyring.SerializePublic());
  ASSERT_TRUE(eval.ok());

  // Shard A and shard B each hold one row whose matched sub-key is 42.
  KeyColumn ka = fx.Keys({42});
  KeyColumn kb = fx.Keys({42});
  PayloadColumns pa = fx.Payloads({1});
  PayloadColumns pb = fx.Payloads({2});

  EncryptedQuery q = fx.Query(42);
  auto ra = fx.backend->evaluate(**eval, q, ka, pa);
  auto rb = fx.backend->evaluate(**eval, q, kb, pb);
  ASSERT_TRUE(ra.ok());
  ASSERT_TRUE(rb.ok());
  std::vector<std::vector<seal::Ciphertext>> shards = {*ra, *rb};
  auto combined = fx.backend->combine(**eval, shards);
  ASSERT_TRUE(combined.ok());

  auto [rows, collided] = fx.Decode(*combined);
  EXPECT_EQ(collided, 0u) << "two distinct rows must not collide in one bucket";
  EXPECT_EQ(rows.size(), 2u)
      << "both shard rows matching the value must be returned";
}

// FIXED BUG (IN with a duplicate operand): WHERE k IN (42, 42). The matcher
// sums per-operand indicators; before the fix a duplicate made the indicator 2
// for the matching row, doubling its payload and a presence of 2 dropped the
// row as collided (verified: 0 rows, 1 collided). BuildEncryptedOperands now
// dedups the operand list, so IN (42, 42) builds a single operand and the row
// comes back clean. This guards the fix end to end through the matcher.
TEST(MatcherBugRepro, DuplicateInOperandIsDedupedAndReturnsTheRow) {
  Fixture fx = Fixture::Make();
  auto eval = fx.backend->load_keys(*fx.keyring.SerializePublic());
  ASSERT_TRUE(eval.ok());

  KeyColumn keys = fx.Keys({42, 7});
  PayloadColumns payload = fx.Payloads({1, 2});

  EncryptedQuery q =
      fx.QueryIn({42, 42}); // IN (42, 42), deduped to one operand
  EXPECT_TRUE(q.extra_operands.empty()) << "the duplicate must be removed";
  auto r = fx.backend->evaluate(**eval, q, keys, payload);
  ASSERT_TRUE(r.ok());
  std::vector<std::vector<seal::Ciphertext>> shards = {*r};
  auto combined = fx.backend->combine(**eval, shards);
  ASSERT_TRUE(combined.ok());

  auto [rows, collided] = fx.Decode(*combined);
  EXPECT_EQ(collided, 0u) << "a single match must not look collided";
  ASSERT_EQ(rows.size(), 1u) << "exactly one row matches key 42";
  EXPECT_EQ(rows[0], Payload(1)) << "payload must not be doubled";
}

// Runs a single-key point query at the given key_bits with the DEFAULT modulus
// and checks the result is decryptable (budget > 0) and the payload is correct.
// Used to probe how deep key_bits can go before the noise budget runs out.
void ExpectPointQueryDecodesAtKeyBits(std::uint32_t kb) {
  CryptoConfig cfg; // default coeff_modulus_bits
  cfg.key_bits = kb;
  auto ctxs = CryptoContext::Create(cfg);
  ASSERT_TRUE(ctxs.ok()) << ctxs.status().message();
  CryptoContext ctx = *ctxs;

  auto steps = opaquedb::core::RequiredGaloisSteps(
      static_cast<std::uint32_t>(ctx.slot_count()), kb);
  ClientKeyring keyring = ClientKeyring::Generate(ctx, steps);

  ASSERT_TRUE(opaquedb::backend::reference::LinkReferenceBackend());
  auto be = BackendRegistry::instance().create("reference", ctx);
  ASSERT_TRUE(be.ok()) << be.status().message();
  std::unique_ptr<PirBackend> backend = *std::move(be);
  auto material = keyring.SerializePublic();
  ASSERT_TRUE(material.ok());
  auto eval = backend->load_keys(*material);
  ASSERT_TRUE(eval.ok()) << eval.status().message();

  KeyColumn keys;
  keys.key_bits = kb;
  keys.keys = {42, 7, 99};
  PayloadColumns payload;
  payload.record_bytes = kRecordBytes;
  payload.bytes_per_slot = kBytesPerSlot;
  payload.rows = {Payload(1), Payload(2), Payload(3)};

  auto blob = opaquedb::crypto::BuildEncryptedOperand(ctx, keyring.public_key(),
                                                      42, kb);
  ASSERT_TRUE(blob.ok());
  auto cts = opaquedb::crypto::DeserializeCiphertexts(ctx, *blob, 1);
  ASSERT_TRUE(cts.ok());
  EncryptedQuery q;
  q.op = Op::kEq;
  q.query = (*cts)[0];
  q.result_buckets = kBuckets;

  auto r = backend->evaluate(**eval, q, keys, payload);
  ASSERT_TRUE(r.ok()) << r.status().message();
  auto combined = backend->combine(**eval, {*r});
  ASSERT_TRUE(combined.ok());

  for (const seal::Ciphertext &plane : *combined) {
    auto budget =
        opaquedb::crypto::NoiseBudgetBits(ctx, keyring.secret_key(), plane);
    ASSERT_TRUE(budget.ok());
    EXPECT_GT(*budget, 0) << "result at key_bits " << kb
                          << " must remain decryptable (budget exhausted means "
                             "silent garbage; config should reject the config)";
  }
  std::string sblob = opaquedb::crypto::SerializeCiphertexts(*combined);
  auto recs = opaquedb::crypto::DecryptResults(ctx, keyring.secret_key(), sblob,
                                               kb, kRecordBytes, kBytesPerSlot,
                                               kBuckets, 0, kBuckets);
  ASSERT_TRUE(recs.ok()) << recs.status().message();
  std::vector<std::vector<std::uint8_t>> rows;
  for (const auto &br : *recs)
    if (br.present && !br.collided)
      rows.push_back(br.record);
  ASSERT_EQ(rows.size(), 1u) << "exactly one row matches key 42";
  EXPECT_EQ(rows[0], Payload(1))
      << "payload must decode correctly at key_bits " << kb;
}

// FIXED BUG (combine validation): CombinePartials summed plane i across shards
// using the max width, with no check that non-empty shards agree on plane
// count. Shards at inconsistent epochs (different schema/geometry) return
// different plane counts, so plane i of one would be added onto a different
// plane of another, silently corrupting the result. combine now rejects a
// plane-count disagreement. This builds two non-empty partial vectors of
// different lengths (fresh zero ciphertexts, no matcher) and expects an error.
TEST(MatcherBugRepro, CombineRejectsShardsThatDisagreeOnPlaneCount) {
  Fixture fx = Fixture::Make();
  auto eval = fx.backend->load_keys(*fx.keyring.SerializePublic());
  ASSERT_TRUE(eval.ok());

  auto encrypt_zero = [&]() {
    std::vector<std::uint64_t> zeros(fx.ctx.slot_count(), 0);
    auto pt = fx.ctx.EncodeBatch(zeros);
    EXPECT_TRUE(pt.ok());
    auto ct = opaquedb::crypto::Encrypt(fx.ctx, fx.keyring.public_key(), *pt);
    EXPECT_TRUE(ct.ok());
    return *ct;
  };
  std::vector<seal::Ciphertext> shard_a(3, encrypt_zero());
  std::vector<seal::Ciphertext> shard_b(5, encrypt_zero());
  std::vector<std::vector<seal::Ciphertext>> shards = {shard_a, shard_b};

  auto combined = fx.backend->combine(**eval, shards);
  EXPECT_FALSE(combined.ok())
      << "mismatched plane counts across shards must be rejected";
}

// key_bits 32 (matcher depth 6) still fits the default 349-bit modulus. This
// guards that the supported deeper key width stays decryptable.
TEST(MatcherBugRepro, DefaultModulusSurvivesKeyBits32) {
  ExpectPointQueryDecodesAtKeyBits(32);
}

// CONFIRMED BUG (config guard): key_bits 64 (matcher depth 7) EXHAUSTS the
// default modulus: the result decrypts to garbage with a noise budget of 0 and
// no error (verified: budget 0, payload bytes scrambled). Config validates that
// key_bits is a power of two <= 64 and fits the slot geometry, but NOT that the
// modulus chain is deep enough for the matcher, so key_bits = 64 is accepted
// and silently corrupts every query. DISABLED because the fix is a guard
// (reject the key_bits/modulus combination at configure time, or a startup
// self-test that probes the budget), not a local patch. Re-enable once the
// guard exists.
TEST(MatcherBugRepro, DISABLED_DefaultModulusIsTooShallowForKeyBits64) {
  ExpectPointQueryDecodesAtKeyBits(64);
}

} // namespace

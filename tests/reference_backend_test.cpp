#include "opaquedb/backend/reference/reference.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "opaquedb/backend/capabilities.h"
#include "opaquedb/backend/pir_backend.h"
#include "opaquedb/backend/registry.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/slot_codec.h"
#include "opaquedb/crypto/context.h"
#include "opaquedb/crypto/key_material.h"
#include "opaquedb/crypto/ops.h"
#include "opaquedb/crypto/serialize.h"

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
using opaquedb::crypto::Decrypt;
using opaquedb::crypto::NoiseBudgetBits;

constexpr std::uint32_t kKeyBits = 16;
constexpr std::uint32_t kBytesPerSlot = 2;
// 40 bytes spans two payload planes (32 bytes per block), exercising the
// multi-plane retrieve and the final truncation.
constexpr std::uint32_t kRecordBytes = 40;

// A deterministic, row-distinct payload so the test can tell which row's bytes
// came back. Only the first 24 bytes are filled, so the second payload plane
// (bytes 32..40 at key_bits=16, bytes_per_slot=2) is entirely zero. That
// exercises the all-zero-plane path, which must not produce a transparent
// ciphertext.
std::vector<std::uint8_t> MakePayload(std::uint64_t key, std::size_t row) {
  std::vector<std::uint8_t> p(kRecordBytes, 0);
  p[0] = static_cast<std::uint8_t>(key & 0xFF);
  p[1] = static_cast<std::uint8_t>((key >> 8) & 0xFF);
  p[2] = static_cast<std::uint8_t>(row + 1);
  for (std::uint32_t i = 3; i < 24; ++i)
    p[i] = static_cast<std::uint8_t>((row * 31 + i) & 0xFF);
  return p;
}

struct Fixture {
  CryptoConfig cfg;
  CryptoContext ctx;
  ClientKeyring keyring;
  std::vector<std::uint64_t> keys = {5, 17, 42, 99};

  static Fixture Make() {
    CryptoConfig cfg; // defaults: poly 16384, key_bits 16
    auto ctx = CryptoContext::Create(cfg);
    EXPECT_TRUE(ctx.ok()) << ctx.status().message();
    // Only the matcher's rotation steps, the same set the client sends. This
    // guards that the step set stays in sync with the backend's rotations.
    auto steps = opaquedb::core::RequiredGaloisSteps(
        static_cast<std::uint32_t>(ctx->slot_count()), kKeyBits);
    return Fixture{cfg, *ctx, ClientKeyring::Generate(*ctx, steps)};
  }

  KeyColumn BuildKeys(const std::vector<std::uint64_t> &ks) const {
    KeyColumn col;
    col.key_bits = kKeyBits;
    col.keys = ks;
    return col;
  }

  PayloadColumns BuildPayload(const std::vector<std::uint64_t> &ks) const {
    PayloadColumns p;
    p.record_bytes = kRecordBytes;
    p.bytes_per_slot = kBytesPerSlot;
    for (std::size_t r = 0; r < ks.size(); ++r)
      p.rows.push_back(MakePayload(ks[r], r));
    return p;
  }

  // Builds the real wire operand and deserializes it back, exercising the
  // client-side encode path the server consumes.
  EncryptedQuery BuildQuery(std::uint64_t value) const {
    auto blob = opaquedb::crypto::BuildEncryptedOperand(
        ctx, keyring.public_key(), value, kKeyBits);
    EXPECT_TRUE(blob.ok()) << blob.status().message();
    auto cts = opaquedb::crypto::DeserializeCiphertexts(ctx, *blob, 1);
    EXPECT_TRUE(cts.ok()) << cts.status().message();
    EXPECT_EQ(cts->size(), 1u);
    EncryptedQuery q;
    q.op = Op::kEq;
    q.query = (*cts)[0];
    return q;
  }

  // Decrypts a combined result (payload planes + presence) into the record
  // bytes, or nullopt when no row matched.
  std::optional<std::vector<std::uint8_t>>
  DecodeResult(const std::vector<seal::Ciphertext> &planes) const {
    std::string blob = opaquedb::crypto::SerializeCiphertexts(planes);
    auto rec = opaquedb::crypto::DecryptRecord(
        ctx, keyring.secret_key(), blob, kKeyBits, kRecordBytes, kBytesPerSlot);
    EXPECT_TRUE(rec.ok()) << rec.status().message();
    return *rec;
  }

  // Builds the query operand for a multi-bucket (LIMIT/OFFSET) retrieve.
  EncryptedQuery BuildQuery(std::uint64_t value, std::uint32_t buckets) const {
    EncryptedQuery q = BuildQuery(value);
    q.result_buckets = buckets;
    return q;
  }

  // The clean records and the collided-bucket count from a window of buckets.
  struct Decoded {
    std::vector<std::vector<std::uint8_t>> rows;
    std::uint32_t collided = 0;
  };
  Decoded DecodeBuckets(const std::vector<seal::Ciphertext> &planes,
                        std::uint32_t buckets, std::uint64_t offset,
                        std::uint64_t limit) const {
    std::string blob = opaquedb::crypto::SerializeCiphertexts(planes);
    auto recs = opaquedb::crypto::DecryptResults(
        ctx, keyring.secret_key(), blob, kKeyBits, kRecordBytes, kBytesPerSlot,
        buckets, offset, limit);
    EXPECT_TRUE(recs.ok()) << recs.status().message();
    Decoded d;
    for (const auto &br : *recs) {
      if (br.collided)
        ++d.collided;
      else if (br.present)
        d.rows.push_back(br.record);
    }
    return d;
  }
};

// Sorts a record list so two retrieval results can be compared as sets (bucket
// order is a hash, not the storage order).
void SortRows(std::vector<std::vector<std::uint8_t>> &rows) {
  std::sort(rows.begin(), rows.end());
}

std::unique_ptr<PirBackend> MakeBackend(const CryptoContext &ctx) {
  EXPECT_TRUE(opaquedb::backend::reference::LinkReferenceBackend());
  auto backend = BackendRegistry::instance().create("reference", ctx);
  EXPECT_TRUE(backend.ok()) << backend.status().message();
  return *std::move(backend);
}

TEST(ReferenceBackend, RegistersWithEqualityCapability) {
  EXPECT_TRUE(opaquedb::backend::reference::LinkReferenceBackend());
  EXPECT_TRUE(BackendRegistry::instance().contains("reference"));
  bool found = false;
  for (const auto &caps : BackendRegistry::instance().list()) {
    if (caps.name == "reference") {
      found = true;
      EXPECT_TRUE(caps.Supports(Op::kEq));
      EXPECT_FALSE(caps.Supports(Op::kRange));
      EXPECT_FALSE(caps.supports_batch);
    }
  }
  EXPECT_TRUE(found);
}

TEST(ReferenceBackend, RetrievesMatchingRowWithPositiveNoiseBudget) {
  Fixture fx = Fixture::Make();
  std::unique_ptr<PirBackend> backend = MakeBackend(fx.ctx);

  auto material = fx.keyring.SerializePublic();
  ASSERT_TRUE(material.ok());
  auto eval = backend->load_keys(*material);
  ASSERT_TRUE(eval.ok()) << eval.status().message();

  KeyColumn keys = fx.BuildKeys(fx.keys);
  PayloadColumns payload = fx.BuildPayload(fx.keys);
  EncryptedQuery query = fx.BuildQuery(42); // row index 2

  auto partials = backend->evaluate(**eval, query, keys, payload);
  ASSERT_TRUE(partials.ok()) << partials.status().message();
  ASSERT_EQ(partials->size(), 3u); // 40 bytes -> 2 planes + presence

  std::vector<std::vector<seal::Ciphertext>> shards;
  shards.push_back(*partials);
  auto combined = backend->combine(**eval, shards);
  ASSERT_TRUE(combined.ok()) << combined.status().message();
  ASSERT_EQ(combined->size(), 3u);

  // The result must remain decryptable: positive invariant noise budget.
  for (const seal::Ciphertext &plane : *combined) {
    auto budget = NoiseBudgetBits(fx.ctx, fx.keyring.secret_key(), plane);
    ASSERT_TRUE(budget.ok());
    EXPECT_GT(*budget, 0) << "result must remain decryptable";
  }

  std::optional<std::vector<std::uint8_t>> got = fx.DecodeResult(*combined);
  ASSERT_TRUE(got.has_value()) << "a matching row must be present";
  EXPECT_EQ(*got, MakePayload(42, 2)) << "wrong row's payload returned";
}

TEST(ReferenceBackend, AbsentKeyReturnsZeroPayload) {
  Fixture fx = Fixture::Make();
  std::unique_ptr<PirBackend> backend = MakeBackend(fx.ctx);
  auto material = fx.keyring.SerializePublic();
  ASSERT_TRUE(material.ok());
  auto eval = backend->load_keys(*material);
  ASSERT_TRUE(eval.ok());

  KeyColumn keys = fx.BuildKeys(fx.keys);
  PayloadColumns payload = fx.BuildPayload(fx.keys);
  EncryptedQuery query = fx.BuildQuery(7); // not in {5,17,42,99}

  auto partials = backend->evaluate(**eval, query, keys, payload);
  ASSERT_TRUE(partials.ok());
  std::optional<std::vector<std::uint8_t>> got = fx.DecodeResult(*partials);
  EXPECT_FALSE(got.has_value()) << "no row should match -> empty result";
}

TEST(ReferenceBackend, CombineSumsAcrossShards) {
  // The matching row sits in one shard; the other shard has only non-matching
  // rows. Combine must return the matching payload.
  Fixture fx = Fixture::Make();
  std::unique_ptr<PirBackend> backend = MakeBackend(fx.ctx);
  auto material = fx.keyring.SerializePublic();
  ASSERT_TRUE(material.ok());
  auto eval = backend->load_keys(*material);
  ASSERT_TRUE(eval.ok());

  // Shard A: rows 0,1. Shard B: rows 2,3 (key 42 at index 0 of shard B).
  std::vector<std::uint64_t> ka = {fx.keys[0], fx.keys[1]};
  std::vector<std::uint64_t> kb = {fx.keys[2], fx.keys[3]};
  KeyColumn a = fx.BuildKeys(ka);
  KeyColumn b = fx.BuildKeys(kb);
  PayloadColumns pa;
  pa.record_bytes = kRecordBytes;
  pa.bytes_per_slot = kBytesPerSlot;
  pa.rows = {MakePayload(fx.keys[0], 0), MakePayload(fx.keys[1], 1)};
  PayloadColumns pb;
  pb.record_bytes = kRecordBytes;
  pb.bytes_per_slot = kBytesPerSlot;
  pb.rows = {MakePayload(fx.keys[2], 0), MakePayload(fx.keys[3], 1)};

  EncryptedQuery query = fx.BuildQuery(42);
  auto ra = backend->evaluate(**eval, query, a, pa);
  auto rb = backend->evaluate(**eval, query, b, pb);
  ASSERT_TRUE(ra.ok());
  ASSERT_TRUE(rb.ok());

  std::vector<std::vector<seal::Ciphertext>> shards = {*ra, *rb};
  auto combined = backend->combine(**eval, shards);
  ASSERT_TRUE(combined.ok());
  std::optional<std::vector<std::uint8_t>> got = fx.DecodeResult(*combined);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, MakePayload(42, 0)) << "matching shard-B row 0";
}

TEST(ReferenceBackend, MultiBucketReturnsAllRowsSharingAKey) {
  // Three rows share key 42; with four buckets they spread to distinct buckets
  // and all come back, none colliding.
  Fixture fx = Fixture::Make();
  std::unique_ptr<PirBackend> backend = MakeBackend(fx.ctx);
  auto material = fx.keyring.SerializePublic();
  ASSERT_TRUE(material.ok());
  auto eval = backend->load_keys(*material);
  ASSERT_TRUE(eval.ok());

  std::vector<std::uint64_t> ks = {42, 7, 42, 99, 42};
  KeyColumn keys = fx.BuildKeys(ks);
  PayloadColumns payload = fx.BuildPayload(ks);
  const std::uint32_t buckets = 4;

  EncryptedQuery query = fx.BuildQuery(42, buckets);
  auto partials = backend->evaluate(**eval, query, keys, payload);
  ASSERT_TRUE(partials.ok()) << partials.status().message();
  std::vector<std::vector<seal::Ciphertext>> shards = {*partials};
  auto combined = backend->combine(**eval, shards);
  ASSERT_TRUE(combined.ok()) << combined.status().message();

  Fixture::Decoded d = fx.DecodeBuckets(*combined, buckets, /*offset=*/0,
                                        /*limit=*/buckets);
  EXPECT_EQ(d.collided, 0u);
  std::vector<std::vector<std::uint8_t>> want = {
      MakePayload(42, 0), MakePayload(42, 2), MakePayload(42, 4)};
  SortRows(want);
  SortRows(d.rows);
  EXPECT_EQ(d.rows, want) << "all rows with key 42 should be returned";
}

TEST(ReferenceBackend, OffsetPagesThroughBucketsWithoutLossOrDuplication) {
  // Paging the bucket window in halves returns each matching row exactly once,
  // and the union equals the full-window result.
  Fixture fx = Fixture::Make();
  std::unique_ptr<PirBackend> backend = MakeBackend(fx.ctx);
  auto material = fx.keyring.SerializePublic();
  ASSERT_TRUE(material.ok());
  auto eval = backend->load_keys(*material);
  ASSERT_TRUE(eval.ok());

  std::vector<std::uint64_t> ks = {42, 42, 42, 42};
  KeyColumn keys = fx.BuildKeys(ks);
  PayloadColumns payload = fx.BuildPayload(ks);
  const std::uint32_t buckets = 4;

  EncryptedQuery query = fx.BuildQuery(42, buckets);
  auto partials = backend->evaluate(**eval, query, keys, payload);
  ASSERT_TRUE(partials.ok()) << partials.status().message();

  Fixture::Decoded full = fx.DecodeBuckets(*partials, buckets, 0, buckets);
  EXPECT_EQ(full.collided, 0u);
  EXPECT_EQ(full.rows.size(), 4u);

  Fixture::Decoded page0 = fx.DecodeBuckets(*partials, buckets, 0, 2);
  Fixture::Decoded page1 = fx.DecodeBuckets(*partials, buckets, 2, 2);
  std::vector<std::vector<std::uint8_t>> paged = page0.rows;
  paged.insert(paged.end(), page1.rows.begin(), page1.rows.end());
  SortRows(paged);
  SortRows(full.rows);
  EXPECT_EQ(paged, full.rows) << "paging by OFFSET must cover all rows once";
}

TEST(ReferenceBackend, MoreDuplicatesThanBucketsCollideAndAreReported) {
  // Five rows share key 42 but there are only four buckets, so at least one
  // bucket must hold two rows and be reported as collided (dropped), not
  // returned as garbage.
  Fixture fx = Fixture::Make();
  std::unique_ptr<PirBackend> backend = MakeBackend(fx.ctx);
  auto material = fx.keyring.SerializePublic();
  ASSERT_TRUE(material.ok());
  auto eval = backend->load_keys(*material);
  ASSERT_TRUE(eval.ok());

  std::vector<std::uint64_t> ks = {42, 42, 42, 42, 42};
  KeyColumn keys = fx.BuildKeys(ks);
  PayloadColumns payload = fx.BuildPayload(ks);
  const std::uint32_t buckets = 4;

  EncryptedQuery query = fx.BuildQuery(42, buckets);
  auto partials = backend->evaluate(**eval, query, keys, payload);
  ASSERT_TRUE(partials.ok()) << partials.status().message();

  Fixture::Decoded d = fx.DecodeBuckets(*partials, buckets, 0, buckets);
  EXPECT_GE(d.collided, 1u) << "a bucket with two rows must be reported";
  EXPECT_EQ(d.rows.size() + d.collided * 2, 5u)
      << "every matching row is either returned clean or in a collided pair";
}

TEST(ReferenceBackend, MultiBucketQueryForZeroValueIgnoresEmptyBlocks) {
  // Empty (padding) blocks hold key 0. A query for value 0 must not treat them
  // as matches: the occupancy mask gates them out. Only the real key-0 row
  // returns.
  Fixture fx = Fixture::Make();
  std::unique_ptr<PirBackend> backend = MakeBackend(fx.ctx);
  auto material = fx.keyring.SerializePublic();
  ASSERT_TRUE(material.ok());
  auto eval = backend->load_keys(*material);
  ASSERT_TRUE(eval.ok());

  std::vector<std::uint64_t> ks = {0, 17, 42};
  KeyColumn keys = fx.BuildKeys(ks);
  PayloadColumns payload = fx.BuildPayload(ks);
  const std::uint32_t buckets = 4;

  EncryptedQuery query = fx.BuildQuery(0, buckets);
  auto partials = backend->evaluate(**eval, query, keys, payload);
  ASSERT_TRUE(partials.ok()) << partials.status().message();

  Fixture::Decoded d = fx.DecodeBuckets(*partials, buckets, 0, buckets);
  EXPECT_EQ(d.collided, 0u);
  ASSERT_EQ(d.rows.size(), 1u) << "only the real key-0 row matches";
  EXPECT_EQ(d.rows[0], MakePayload(0, 0));
}

} // namespace

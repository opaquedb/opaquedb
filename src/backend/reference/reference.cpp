#include "opaquedb/backend/reference/reference.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "opaquedb/backend/registry.h"
#include "opaquedb/core/key_codec.h"
#include "opaquedb/core/slot_codec.h"
#include "opaquedb/crypto/ops.h"

namespace opaquedb::backend::reference {
namespace {

using backend::BackendCapabilities;
using backend::EncryptedQuery;
using backend::EvalContext;
using backend::KeyColumn;
using backend::Op;
using backend::PayloadColumns;
using backend::Privacy;
using backend::Scheme;
using backend::SecurityModel;
using backend::ServerModel;

// The client keys bound to the node context, used by evaluate.
class ReferenceEvalContext : public EvalContext {
public:
  explicit ReferenceEvalContext(crypto::EvalKeys keys)
      : keys_(std::move(keys)) {}

  const crypto::EvalKeys &keys() const { return keys_; }

private:
  crypto::EvalKeys keys_;
};

bool IsPowerOfTwo(std::uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

int Log2(std::uint32_t n) {
  int l = 0;
  while ((1u << l) < n)
    ++l;
  return l;
}

} // namespace

bool LinkReferenceBackend() { return true; }

ReferenceBackend::ReferenceBackend(crypto::CryptoContext ctx)
    : ctx_(std::move(ctx)),
      evaluator_(std::make_shared<seal::Evaluator>(ctx_.seal())) {}

BackendCapabilities ReferenceBackend::StaticCapabilities() {
  BackendCapabilities caps;
  caps.name = "reference";
  caps.scheme = Scheme::kBfv;
  caps.server_model = ServerModel::kSingleServer;
  caps.security = SecurityModel::kSemiHonest;
  caps.operators = {Op::kEq};
  caps.privacy_modes = {Privacy::kQueryPrivate};
  caps.supports_batch = false;
  return caps;
}

BackendCapabilities ReferenceBackend::capabilities() const {
  return StaticCapabilities();
}

absl::StatusOr<std::unique_ptr<EvalContext>>
ReferenceBackend::load_keys(const crypto::KeyMaterial &keys) {
  absl::StatusOr<crypto::EvalKeys> loaded = crypto::LoadKeyMaterial(ctx_, keys);
  if (!loaded.ok())
    return loaded.status();
  if (loaded->galois_keys.size() == 0) {
    return absl::InvalidArgumentError(
        "reference backend requires Galois keys; register a keyring generated "
        "with Galois keys");
  }
  return std::unique_ptr<EvalContext>(
      std::make_unique<ReferenceEvalContext>(*std::move(loaded)));
}

absl::StatusOr<std::vector<seal::Ciphertext>>
ReferenceBackend::evaluate(EvalContext &ctx, const EncryptedQuery &query,
                           const KeyColumn &keys,
                           const PayloadColumns &payload) {
  auto *ref = dynamic_cast<ReferenceEvalContext *>(&ctx);
  if (ref == nullptr) {
    return absl::InvalidArgumentError(
        "reference backend: eval context was not produced by load_keys");
  }
  if (query.op != Op::kEq) {
    return absl::UnimplementedError(
        absl::StrCat("reference backend supports only equality, got op ",
                     backend::ToString(query.op)));
  }
  if (keys.size() != payload.size()) {
    return absl::InvalidArgumentError(
        absl::StrCat("key column has ", keys.size(), " rows but payload has ",
                     payload.size()));
  }

  const std::uint32_t kb = keys.key_bits;
  const std::uint32_t bps = payload.bytes_per_slot;
  const std::uint32_t record_bytes = payload.record_bytes;
  if (kb == 0 || !IsPowerOfTwo(kb)) {
    return absl::InvalidArgumentError(
        absl::StrCat("key_bits must be a power of two, got ", kb));
  }
  if (bps == 0 || record_bytes == 0) {
    return absl::InvalidArgumentError(
        "payload geometry (record_bytes, bytes_per_slot) must be positive");
  }

  const std::size_t total = keys.size();
  if (total == 0) {
    return std::vector<seal::Ciphertext>{}; // empty shard: no partials
  }

  const std::size_t N = ctx_.slot_count();
  const std::size_t row = N / 2;
  if (kb > row || row % kb != 0) {
    return absl::InvalidArgumentError(absl::StrCat(
        "key_bits ", kb, " does not divide the slot row size ", row));
  }
  const std::size_t blocks_per_row = row / kb;
  const std::size_t records_per_ct = 2 * blocks_per_row;
  const int levels = Log2(kb);
  const std::uint32_t bytes_per_block = core::PayloadBytesPerBlock(kb, bps);
  const std::uint32_t planes = core::PayloadPlaneCount(record_bytes, kb, bps);

  // The slot offset of record j's block within a batch ciphertext. Records
  // [0, blocks_per_row) sit in row 0, the rest in row 1, each in its own block.
  auto block_start = [&](std::size_t j) -> std::size_t {
    return j < blocks_per_row ? j * kb : row + (j - blocks_per_row) * kb;
  };

  const seal::RelinKeys &relin = ref->keys().relin_keys;
  const seal::GaloisKeys &gal = ref->keys().galois_keys;

  try {
    // Batch-independent plaintexts.
    std::vector<std::uint64_t> ones(N, 1);
    absl::StatusOr<seal::Plaintext> ones_pt = ctx_.EncodeBatch(ones);
    if (!ones_pt.ok())
      return ones_pt.status();

    std::vector<std::uint64_t> start_mask(N, 0);
    for (std::size_t s = 0; s < N; ++s)
      start_mask[s] = (s % kb == 0) ? 1 : 0;
    absl::StatusOr<seal::Plaintext> start_pt = ctx_.EncodeBatch(start_mask);
    if (!start_pt.ok())
      return start_pt.status();

    // A fresh encryption of zero, used to seed every plane's accumulator. This
    // makes each returned plane a valid (non-transparent) ciphertext even when
    // a plane carries no data (e.g. a record's zero-padding region): such
    // planes contribute nothing and we never multiply by an all-zero plaintext,
    // which SEAL rejects as a transparent ciphertext.
    absl::StatusOr<seal::Plaintext> zero_pt =
        ctx_.EncodeBatch(std::vector<std::uint64_t>(N, 0));
    if (!zero_pt.ok())
      return zero_pt.status();
    absl::StatusOr<seal::Ciphertext> zero_ct =
        crypto::Encrypt(ctx_, ref->keys().public_key, *zero_pt);
    if (!zero_ct.ok())
      return zero_ct.status();

    // One ciphertext per payload plane, plus a trailing "presence" ciphertext
    // that accumulates sum_r b_r (the number of matching records). The client
    // reads presence first: a count of zero means no row matched, so it returns
    // an empty result instead of decoding the all-zero payload as a real
    // record.
    std::vector<seal::Ciphertext> acc(planes + 1, *zero_ct);
    const std::uint32_t presence_idx = planes;

    for (std::size_t base = 0; base < total; base += records_per_ct) {
      const std::size_t batch = std::min(records_per_ct, total - base);

      // Build the key plaintext: each record's bits in its block.
      std::vector<std::uint64_t> key_slots(N, 0);
      for (std::size_t j = 0; j < batch; ++j) {
        const std::vector<std::uint64_t> bits =
            core::KeyToBits(keys.keys[base + j], kb);
        const std::size_t at = block_start(j);
        for (std::uint32_t b = 0; b < kb; ++b)
          key_slots[at + b] = bits[b];
      }
      absl::StatusOr<seal::Plaintext> key_pt = ctx_.EncodeBatch(key_slots);
      if (!key_pt.ok())
        return key_pt.status();

      // sel = 1 - (query - key)^2, AND-reduced across each block.
      seal::Ciphertext sel = query.query;
      evaluator_->sub_plain_inplace(sel, *key_pt);
      evaluator_->square_inplace(sel);
      evaluator_->relinearize_inplace(sel, relin);
      evaluator_->negate_inplace(sel);
      evaluator_->add_plain_inplace(sel, *ones_pt);
      for (int i = 0; i < levels; ++i) {
        seal::Ciphertext rot;
        evaluator_->rotate_rows(sel, 1 << i, gal, rot);
        evaluator_->multiply_inplace(sel, rot);
        evaluator_->relinearize_inplace(sel, relin);
      }
      // Keep the indicator at block starts, then broadcast it across each block
      // by a doubling prefix-sum. The window grows to exactly key_bits wide,
      // and block starts are key_bits apart, so each slot's window holds
      // exactly one start: no cross-block bleed, and no plaintext multiplies to
      // spend noise.
      evaluator_->multiply_plain_inplace(sel, *start_pt);
      for (int i = 0; i < levels; ++i) {
        seal::Ciphertext rot;
        evaluator_->rotate_rows(sel, -(1 << i), gal, rot);
        evaluator_->add_inplace(sel, rot);
      }

      for (std::uint32_t p = 0; p < planes; ++p) {
        // Pack plane p of each record's payload into its block.
        std::vector<std::uint64_t> pay_slots(N, 0);
        const std::uint32_t lo = p * bytes_per_block;
        const std::uint32_t hi =
            std::min(record_bytes, (p + 1) * bytes_per_block);
        for (std::size_t j = 0; j < batch; ++j) {
          const std::vector<std::uint8_t> &rec = payload.rows[base + j];
          if (lo >= rec.size())
            continue;
          const std::uint32_t end = std::min<std::uint32_t>(
              hi, static_cast<std::uint32_t>(rec.size()));
          absl::StatusOr<std::vector<std::uint64_t>> packed =
              core::PackBytesToSlots(
                  std::span<const std::uint8_t>(rec.data() + lo, end - lo),
                  bps);
          if (!packed.ok())
            return packed.status();
          const std::size_t at = block_start(j);
          for (std::size_t s = 0; s < packed->size(); ++s)
            pay_slots[at + s] = (*packed)[s];
        }
        // A plane with no data in this batch contributes nothing. Skipping it
        // also avoids multiply_plain by an all-zero plaintext (a transparent
        // ciphertext). acc[p] is already a valid encryption of zero.
        bool any = false;
        for (std::uint64_t v : pay_slots)
          if (v != 0) {
            any = true;
            break;
          }
        if (!any)
          continue;

        absl::StatusOr<seal::Plaintext> pay_pt = ctx_.EncodeBatch(pay_slots);
        if (!pay_pt.ok())
          return pay_pt.status();

        seal::Ciphertext out = sel;
        evaluator_->multiply_plain_inplace(out, *pay_pt);
        // Sum every block within a row into that row's block 0. We do not
        // rotate columns to merge the two rows (that would need an extra Galois
        // key); the matched payload lands in block 0 of whichever row holds it,
        // and the client sums the two row-0 blocks (only one is nonzero).
        for (std::size_t step = kb; step < row; step <<= 1) {
          seal::Ciphertext rot;
          evaluator_->rotate_rows(out, static_cast<int>(step), gal, rot);
          evaluator_->add_inplace(out, rot);
        }
        evaluator_->add_inplace(acc[p], out);
      }

      // Presence: block-sum the broadcast indicator so block 0 holds sum_r b_r
      // for this batch. No plaintext multiply, so this never goes transparent.
      seal::Ciphertext pres = sel;
      for (std::size_t step = kb; step < row; step <<= 1) {
        seal::Ciphertext rot;
        evaluator_->rotate_rows(pres, static_cast<int>(step), gal, rot);
        evaluator_->add_inplace(pres, rot);
      }
      evaluator_->add_inplace(acc[presence_idx], pres);
    }

    return acc;
  } catch (const std::exception &e) {
    return absl::InternalError(
        absl::StrCat("reference backend evaluate failed: ", e.what()));
  }
}

absl::StatusOr<std::vector<seal::Ciphertext>>
ReferenceBackend::combine(EvalContext &ctx,
                          std::vector<std::vector<seal::Ciphertext>> partials) {
  if (dynamic_cast<ReferenceEvalContext *>(&ctx) == nullptr) {
    return absl::InvalidArgumentError(
        "reference backend: eval context was not produced by load_keys");
  }
  // Sum partials plane-wise across shards. Each non-empty shard returns the
  // same number of planes; an empty shard returns none, so add up to the max
  // width.
  std::size_t width = 0;
  for (const auto &shard : partials)
    width = std::max(width, shard.size());

  try {
    std::vector<seal::Ciphertext> out;
    out.reserve(width);
    for (std::size_t i = 0; i < width; ++i) {
      bool have = false;
      seal::Ciphertext acc;
      for (const auto &shard : partials) {
        if (i >= shard.size())
          continue;
        if (!have) {
          acc = shard[i];
          have = true;
        } else {
          evaluator_->add_inplace(acc, shard[i]);
        }
      }
      if (have)
        out.push_back(std::move(acc));
    }
    return out;
  } catch (const std::exception &e) {
    return absl::InternalError(
        absl::StrCat("reference backend combine failed: ", e.what()));
  }
}

namespace {

// Self-registration. Linking this library and referencing LinkReferenceBackend
// pulls in this translation unit, running the registration exactly once.
const bool kRegistered = [] {
  backend::BackendRegistry::instance().register_backend(
      "reference",
      [](const crypto::CryptoContext &ctx)
          -> std::unique_ptr<backend::PirBackend> {
        return std::make_unique<ReferenceBackend>(ctx);
      },
      ReferenceBackend::StaticCapabilities());
  return true;
}();

} // namespace

} // namespace opaquedb::backend::reference

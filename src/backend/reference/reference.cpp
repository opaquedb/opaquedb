#include "opaquedb/backend/reference/reference.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <thread>
#include <unordered_map>
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

// A fixed integer mix (the splitmix64 finalizer) that spreads a key value over
// the bucket range. Multi-match placement starts each key's rows at bucket
// Mix(key) % buckets and walks forward, so rows that share a key land in
// distinct buckets while different keys are spread evenly for dense packing.
// This is plaintext, query-independent, and deterministic, so every query and
// every shard place the same key the same way.
std::uint64_t Mix(std::uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
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
  caps.operators = {Op::kEq, Op::kNe};
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
  if (query.op != Op::kEq && query.op != Op::kNe) {
    return absl::UnimplementedError(
        absl::StrCat("reference backend supports only '=' and '<>', got op ",
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
  const int levels = Log2(kb);
  const std::uint32_t bytes_per_block = core::PayloadBytesPerBlock(kb, bps);
  const std::uint32_t planes = core::PayloadPlaneCount(record_bytes, kb, bps);

  // Result buckets. 1 is the single-match path: the block-sum runs to the end
  // so one bucket holds the sum of every block (correct when at most one row
  // matches). A larger power of two stops the block-sum early, leaving that
  // many partial sums per row, and spreads rows that share a key across
  // distinct buckets so a window of matches can be returned. It must divide the
  // blocks in one row.
  const std::uint32_t buckets =
      query.result_buckets == 0 ? 1 : query.result_buckets;
  if (!IsPowerOfTwo(buckets) ||
      buckets > static_cast<std::uint32_t>(blocks_per_row)) {
    return absl::InvalidArgumentError(
        absl::StrCat("result_buckets must be a power of two in [1, ",
                     blocks_per_row, "], got ", buckets));
  }
  const std::size_t seg = blocks_per_row / buckets; // blocks per bucket per row
  const std::size_t slots_per_bucket_ct = 2 * seg;  // bucket capacity per batch
  const std::size_t stride = core::BucketStride(static_cast<std::uint32_t>(N),
                                                buckets); // = row/buckets

  // Lay out every row into a (batch ciphertext, slot) position. A row that is
  // the i-th of its key goes to bucket (Mix(key) + i) % buckets, then to the
  // next free block slot inside that bucket; the first seg slots of a bucket
  // sit in row 0 of the batch, the rest in row 1. Rows sharing a key thus land
  // in distinct buckets (no collision until a key has more than `buckets`
  // rows), and different keys spread evenly so packing stays dense. With
  // buckets == 1 this is exactly the original sequential packing. The placement
  // is grouped by batch so each batch ciphertext is assembled once.
  std::vector<std::vector<std::pair<std::size_t, std::size_t>>> batched_rows;
  {
    std::unordered_map<std::uint64_t, std::uint32_t> key_seq;
    std::vector<std::size_t> occ(buckets, 0);
    const std::uint32_t mask = buckets - 1;
    for (std::size_t r = 0; r < total; ++r) {
      std::size_t g = 0;
      if (buckets > 1) {
        const std::uint32_t i = key_seq[keys.keys[r]]++;
        g = (static_cast<std::uint32_t>(Mix(keys.keys[r])) + i) & mask;
      }
      const std::size_t o = occ[g]++;
      const std::size_t b = o / slots_per_bucket_ct;
      const std::size_t k = o % slots_per_bucket_ct;
      const std::size_t blk = g * seg + (k < seg ? k : k - seg);
      const std::size_t slot = (k < seg) ? blk * kb : row + blk * kb;
      if (b >= batched_rows.size())
        batched_rows.resize(b + 1);
      batched_rows[b].emplace_back(r, slot);
    }
  }

  const seal::RelinKeys &relin = ref->keys().relin_keys;
  const seal::GaloisKeys &gal = ref->keys().galois_keys;

  try {
    // Batch-independent plaintexts.
    std::vector<std::uint64_t> ones(N, 1);
    absl::StatusOr<seal::Plaintext> ones_pt = ctx_.EncodeBatch(ones);
    if (!ones_pt.ok())
      return ones_pt.status();

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

    // Drops a ciphertext one prime down the modulus chain if it is not already
    // at the last level. The multiplicative depth is spent by the time the
    // indicator is built, so the payload multiply and the rotation-heavy
    // block-sum no longer need the top modulus; a smaller modulus makes every
    // remaining rotation (a Galois key-switch) and multiply cheaper. The
    // accumulators are dropped the same single step so the final adds line up.
    const seal::Evaluator &ev = *evaluator_;
    const seal::SEALContext &seal_ctx = ctx_.seal();
    auto drop_one = [&ev, &seal_ctx](seal::Ciphertext &c) {
      auto data = seal_ctx.get_context_data(c.parms_id());
      if (data && data->next_context_data())
        ev.mod_switch_to_next_inplace(c);
    };
    drop_one(*zero_ct);

    // One ciphertext per payload plane, plus a trailing "presence" ciphertext
    // that accumulates sum_r b_r (the number of matching records). The client
    // reads presence first: a count of zero means no row matched, so it returns
    // an empty result instead of decoding the all-zero payload as a real
    // record.
    const std::uint32_t presence_idx = planes;
    const std::vector<seal::Ciphertext> seed(planes + 1, *zero_ct);

    // Evaluates one batch into its own accumulator vector `local` (pre-seeded
    // with encryptions of zero at the dropped level). Batches are independent,
    // so several run concurrently, each owning its scratch and writing only its
    // own `local`; the SEAL Evaluator and BatchEncoder are used read-only and
    // are safe to share. Exceptions are caught here so a worker thread never
    // escapes with one.
    auto process_batch =
        [&](const std::vector<std::pair<std::size_t, std::size_t>> &items,
            std::vector<seal::Ciphertext> &local) -> absl::Status {
      try {
        // Build the key plaintext (each row's bits at its assigned slot) and
        // the occupancy mask (a 1 at every occupied block start). The mask, not
        // a static every-block-start mask, gates the indicator: empty block
        // slots hold key 0, which would otherwise spuriously match a query for
        // value 0 and inflate the count, so only occupied blocks survive.
        std::vector<std::uint64_t> key_slots(N, 0);
        std::vector<std::uint64_t> start_mask(N, 0);
        for (const std::pair<std::size_t, std::size_t> &item : items) {
          const std::vector<std::uint64_t> bits =
              core::KeyToBits(keys.keys[item.first], kb);
          const std::size_t at = item.second;
          for (std::uint32_t b = 0; b < kb; ++b)
            key_slots[at + b] = bits[b];
          start_mask[at] = 1;
        }
        absl::StatusOr<seal::Plaintext> key_pt = ctx_.EncodeBatch(key_slots);
        if (!key_pt.ok())
          return key_pt.status();
        absl::StatusOr<seal::Plaintext> start_pt = ctx_.EncodeBatch(start_mask);
        if (!start_pt.ok())
          return start_pt.status();

        // The per-row equality indicator for one operand: eq = 1 - (op -
        // key)^2, AND-reduced across each block so a block is 1 only when every
        // bit matches.
        auto equality_indicator =
            [&](const seal::Ciphertext &operand) -> seal::Ciphertext {
          seal::Ciphertext s = operand;
          ev.sub_plain_inplace(s, *key_pt);
          ev.square_inplace(s);
          ev.relinearize_inplace(s, relin);
          ev.negate_inplace(s);
          ev.add_plain_inplace(s, *ones_pt);
          for (int i = 0; i < levels; ++i) {
            seal::Ciphertext rot;
            ev.rotate_rows(s, 1 << i, gal, rot);
            ev.multiply_inplace(s, rot);
            ev.relinearize_inplace(s, relin);
          }
          return s;
        };

        // sel is the union of the per-operand indicators. A key equals at most
        // one operand, so the indicators are disjoint and summing them yields a
        // 0/1 indicator without spending extra depth. One operand is a point
        // query; several is IN / same-column OR.
        seal::Ciphertext sel = equality_indicator(query.query);
        for (const seal::Ciphertext &extra : query.extra_operands) {
          seal::Ciphertext eq = equality_indicator(extra);
          ev.add_inplace(sel, eq);
        }
        // The indicator is final: no more ct*ct multiplies follow, so drop a
        // prime before the occupancy mask, the broadcast, and the block-sum.
        drop_one(sel);
        // Keep the indicator at occupied block starts. After this mask sel is
        // the match indicator (1 at an occupied start whose key equals an
        // operand, else 0).
        ev.multiply_plain_inplace(sel, *start_pt);
        // For '<>' invert the indicator at occupied starts: ne = 1 - eq. The
        // mask is 0 off the starts, so negating and adding it back leaves
        // 1 - eq at each occupied start and 0 elsewhere. This is plaintext
        // arithmetic on a final ciphertext: no extra multiplicative depth, so
        // '<>' costs exactly what '=' costs. (Empty padding blocks hold key 0
        // but are not occupied starts, so they stay 0 and never match.)
        if (query.op == Op::kNe) {
          ev.negate_inplace(sel);
          ev.add_plain_inplace(sel, *start_pt);
        }
        // Broadcast the indicator across each block by a doubling prefix-sum.
        // The window grows to exactly key_bits wide, and block starts are
        // key_bits apart, so each slot's window holds exactly one start: no
        // cross-block bleed, and no plaintext multiplies to spend noise.
        for (int i = 0; i < levels; ++i) {
          seal::Ciphertext rot;
          ev.rotate_rows(sel, -(1 << i), gal, rot);
          ev.add_inplace(sel, rot);
        }

        for (std::uint32_t p = 0; p < planes; ++p) {
          // Pack plane p of each row's payload into its assigned block.
          std::vector<std::uint64_t> pay_slots(N, 0);
          const std::uint32_t lo = p * bytes_per_block;
          const std::uint32_t hi =
              std::min(record_bytes, (p + 1) * bytes_per_block);
          for (const std::pair<std::size_t, std::size_t> &item : items) {
            const std::vector<std::uint8_t> &rec = payload.rows[item.first];
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
            const std::size_t at = item.second;
            for (std::size_t s = 0; s < packed->size(); ++s)
              pay_slots[at + s] = (*packed)[s];
          }
          // A plane with no data in this batch contributes nothing. Skipping it
          // also avoids multiply_plain by an all-zero plaintext (a transparent
          // ciphertext). local[p] is already a valid encryption of zero.
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
          ev.multiply_plain_inplace(out, *pay_pt);
          // Sum the blocks within each bucket into the bucket's first block.
          // The doubling sum stops at the bucket width (stride): with buckets
          // == 1 that is the whole row (every block folds into block 0); with
          // more buckets it leaves one partial sum per bucket, spaced stride
          // apart. We do not rotate columns to merge the two rows; the client
          // sums the two rows' bucket g (only one side is nonzero per match).
          for (std::size_t step = kb; step < stride; step <<= 1) {
            seal::Ciphertext rot;
            ev.rotate_rows(out, static_cast<int>(step), gal, rot);
            ev.add_inplace(out, rot);
          }
          ev.add_inplace(local[p], out);
        }

        // Presence: the same per-bucket sum of the broadcast indicator, so each
        // bucket's first block holds the number of rows matching in that
        // bucket. No plaintext multiply, so this never goes transparent.
        seal::Ciphertext pres = sel;
        for (std::size_t step = kb; step < stride; step <<= 1) {
          seal::Ciphertext rot;
          ev.rotate_rows(pres, static_cast<int>(step), gal, rot);
          ev.add_inplace(pres, rot);
        }
        ev.add_inplace(local[presence_idx], pres);
        return absl::OkStatus();
      } catch (const std::exception &e) {
        return absl::InternalError(
            absl::StrCat("reference backend batch failed: ", e.what()));
      }
    };

    // Run the batches. One batch (small tables) stays serial; otherwise split
    // them across worker threads, each with its own accumulator, then reduce.
    unsigned hw = std::thread::hardware_concurrency();
    std::size_t nthreads = std::min<std::size_t>(
        batched_rows.size(), hw == 0 ? 1u : static_cast<std::size_t>(hw));

    std::vector<seal::Ciphertext> acc = seed;
    if (nthreads <= 1) {
      for (const auto &items : batched_rows) {
        absl::Status s = process_batch(items, acc);
        if (!s.ok())
          return s;
      }
    } else {
      std::vector<std::vector<seal::Ciphertext>> locals(nthreads, seed);
      std::vector<absl::Status> statuses(nthreads, absl::OkStatus());
      std::vector<std::thread> pool;
      pool.reserve(nthreads);
      for (std::size_t t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t]() {
          for (std::size_t i = t; i < batched_rows.size(); i += nthreads) {
            absl::Status s = process_batch(batched_rows[i], locals[t]);
            if (!s.ok()) {
              statuses[t] = s;
              return;
            }
          }
        });
      }
      for (std::thread &th : pool)
        th.join();
      for (const absl::Status &s : statuses)
        if (!s.ok())
          return s;
      // Reduce the per-thread accumulators plane-wise. Adding the extra
      // encryptions of zero costs negligible noise.
      for (std::size_t t = 0; t < nthreads; ++t)
        for (std::uint32_t p = 0; p <= planes; ++p)
          ev.add_inplace(acc[p], locals[t][p]);
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

  // Every non-empty shard must agree on the plane count. A disagreement means
  // the shards answered from inconsistent epochs (different schema or record
  // geometry); summing plane i of one against plane i of another would add a
  // payload plane onto a presence plane and silently corrupt the result. Reject
  // it instead. (Empty shards contribute nothing and are skipped.)
  for (const auto &shard : partials) {
    if (!shard.empty() && shard.size() != width) {
      return absl::InvalidArgumentError(absl::StrCat(
          "reference backend combine: shards disagree on plane count (", width,
          " vs ", shard.size(),
          "); the shards answered from inconsistent epochs"));
    }
  }

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

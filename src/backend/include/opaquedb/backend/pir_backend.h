#ifndef OPAQUEDB_BACKEND_PIR_BACKEND_H_
#define OPAQUEDB_BACKEND_PIR_BACKEND_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "absl/status/statusor.h"
#include "opaquedb/backend/capabilities.h"
#include "opaquedb/crypto/context.h"
#include "opaquedb/crypto/key_material.h"
#include "seal/seal.h"

// The backend interface is the extensibility spine. It must not assume one
// scheme, one server model, or single-item return. The query path holds a
// PirBackend through this interface and never names a concrete backend.
//
// Vocabulary is FHE-correct: the encrypted query is one or more ciphertexts,
// the key column and payload columns are the data the predicate runs over, and
// the data is passed in, not a hard assumption, so a future DataPrivate mode
// reuses the same evaluate path with encrypted data.

namespace opaquedb::backend {

// Per-client evaluation state: the client's public and evaluation keys bound to
// the node's FHE context, plus any backend scratch. Backends subclass this. It
// is opaque to the layers above; load_keys produces it and evaluate consumes
// it.
class EvalContext {
public:
  virtual ~EvalContext() = default;
};

// The encrypted predicate operand handed to a shard. For the reference backend
// the query is a single ciphertext: the lookup value binary-expanded into
// key_bits bits and tiled across every BatchEncoder slot. Other backends carry
// their own ciphertext shape here.
struct EncryptedQuery {
  Op op = Op::kEq;
  seal::Ciphertext query;
};

// The column the predicate matches against, one segment's worth of rows. Held
// as plaintext key values; the backend binary-expands each into key_bits slots.
// This is the data: plaintext in QueryPrivate mode, ciphertext in a future
// DataPrivate mode, which is why evaluate takes it as a parameter.
struct KeyColumn {
  std::uint32_t key_bits = 0;      // bits per key the matcher compares
  std::vector<std::uint64_t> keys; // one key value per row, < 2^key_bits

  std::size_t size() const { return keys.size(); }
};

// The payload to retrieve, one segment's worth of rows. Each row is the fixed
// record_bytes of payload as raw bytes; the backend packs them into slots
// (bytes_per_slot per slot) and lays each record across key_bits-slot blocks,
// split into ceil(record_bytes / (key_bits * bytes_per_slot)) planes.
struct PayloadColumns {
  std::uint32_t record_bytes = 0;
  std::uint32_t bytes_per_slot = 0;
  std::vector<std::vector<std::uint8_t>> rows;

  std::size_t size() const { return rows.size(); }
};

// The algorithm strategy. A backend self-registers a factory with the registry,
// the planner picks one by name or capabilities, and the query path drives it.
class PirBackend {
public:
  virtual ~PirBackend() = default;

  virtual BackendCapabilities capabilities() const = 0;

  // Binds a client's serialized public and evaluation keys to this backend's
  // context, validating them against the FHE parameters before use.
  virtual absl::StatusOr<std::unique_ptr<EvalContext>>
  load_keys(const crypto::KeyMaterial &keys) = 0;

  // Runs the encrypted predicate over one shard segment and returns encrypted
  // partials, one ciphertext per payload plane. Returns a list so multi-plane
  // (and future multi-item) return stays non-breaking.
  virtual absl::StatusOr<std::vector<seal::Ciphertext>>
  evaluate(EvalContext &ctx, const EncryptedQuery &query, const KeyColumn &keys,
           const PayloadColumns &payload) = 0;

  // Sums partials from many shards under encryption. Stays correct as the
  // number of partials grows, so batch PIR can reuse it per bucket.
  virtual absl::StatusOr<std::vector<seal::Ciphertext>>
  combine(EvalContext &ctx,
          std::vector<std::vector<seal::Ciphertext>> partials) = 0;
};

} // namespace opaquedb::backend

#endif // OPAQUEDB_BACKEND_PIR_BACKEND_H_

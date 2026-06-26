#ifndef OPAQUEDB_STORAGE_STAGING_H_
#define OPAQUEDB_STORAGE_STAGING_H_

#include <cstdint>
#include <span>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/core/schema.h"

// The staging epoch accumulates rows before they are published as immutable
// segments. Each row has a match part (the key value packed little-endian) and
// a payload part (the fixed-size record). Storage holds these as opaque bytes
// and validates only their sizes; it does not interpret them.

namespace opaquedb::storage {

// The SIMD slot layout, in plain integers so storage stays free of SEAL. These
// make the slot width and record stride explicit rather than magic numbers.
struct SlotGeometry {
  std::uint32_t slot_count = 0;     // N, the BatchEncoder slots per plaintext
  std::uint32_t bytes_per_slot = 0; // payload bytes packed into one slot
  std::uint32_t record_bytes = 0;   // payload record stride, from config

  absl::Status Validate() const;
};

// Bytes a stored key of the given bit length occupies, rounding up. The matcher
// holds the key value, not a codeword, so this is ceil(key_bits / 8).
inline std::uint32_t MatchRecordBytes(std::uint32_t key_bits) {
  return (key_bits + 7) / 8;
}

struct Row {
  std::vector<std::uint8_t> match;
  std::vector<std::uint8_t> payload;
};

// Serializes a row for the WAL: length-prefixed match then payload. Used so a
// replayed log reconstructs the exact rows.
std::vector<std::uint8_t> SerializeRow(const Row &row);

// Parses a row produced by SerializeRow. Validates every length against the
// available bytes, so a malformed entry is rejected rather than over-read.
absl::StatusOr<Row> ParseRow(std::span<const std::uint8_t> bytes);

// Accumulates validated rows for one epoch.
class StagingEpoch {
public:
  StagingEpoch(core::Schema schema, std::uint32_t key_bits,
               SlotGeometry geometry)
      : schema_(std::move(schema)), key_bits_(key_bits), geometry_(geometry) {}

  // Checks the schema, the key parameter, and the geometry are coherent.
  absl::Status Validate() const;

  // Appends a row, rejecting it unless the match part is exactly the key size
  // and the payload is exactly the record stride.
  absl::Status AppendRow(Row row);

  const core::Schema &schema() const { return schema_; }
  std::uint32_t key_bits() const { return key_bits_; }
  const SlotGeometry &geometry() const { return geometry_; }
  const std::vector<Row> &rows() const { return rows_; }
  // The match record packs one key per searchable column (the primary key plus
  // every secondary index), so the stride scales with the searchable count.
  std::uint32_t match_record_bytes() const {
    return static_cast<std::uint32_t>(schema_.SearchableCount()) *
           MatchRecordBytes(key_bits_);
  }

private:
  core::Schema schema_;
  std::uint32_t key_bits_;
  SlotGeometry geometry_;
  std::vector<Row> rows_;
};

} // namespace opaquedb::storage

#endif // OPAQUEDB_STORAGE_STAGING_H_

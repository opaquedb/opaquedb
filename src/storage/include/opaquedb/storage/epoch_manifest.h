#ifndef OPAQUEDB_STORAGE_EPOCH_MANIFEST_H_
#define OPAQUEDB_STORAGE_EPOCH_MANIFEST_H_

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/storage/staging.h"

// The epoch manifest is the machine-written description of one immutable epoch.
// It carries a format version so future changes stay compatible, the epoch
// version, the schema (the single source of truth for the table layout), the
// key bit width the matcher uses, the slot geometry, and a descriptor for each
// segment file with its stride, count, and CRC. It is written as JSON.

namespace opaquedb::storage {

inline constexpr std::uint32_t kManifestFormatVersion = 1;

struct SegmentDescriptor {
  std::string file;
  std::uint32_t record_bytes = 0;
  std::uint64_t record_count = 0;
  std::uint32_t crc32 = 0;
};

struct EpochManifest {
  std::uint32_t format_version = kManifestFormatVersion;
  std::uint64_t epoch_version = 0;
  core::Schema schema;
  std::uint32_t key_bits = 0;
  SlotGeometry geometry;
  SegmentDescriptor match_segment;
  SegmentDescriptor payload_segment;

  // Structural checks: supported version, valid schema, code params, geometry,
  // and the match and payload segments agree on the row count.
  absl::Status Validate() const;
};

// Serializes a manifest to pretty JSON. Callers should Validate first.
std::string SerializeManifest(const EpochManifest &manifest);

// Parses and validates a manifest from JSON text. Missing or wrong-typed fields
// are an error, not a default, so a corrupt manifest never yields a usable but
// wrong epoch.
absl::StatusOr<EpochManifest> ParseManifest(std::string_view json);

} // namespace opaquedb::storage

#endif // OPAQUEDB_STORAGE_EPOCH_MANIFEST_H_

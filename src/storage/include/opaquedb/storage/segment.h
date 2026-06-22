#ifndef OPAQUEDB_STORAGE_SEGMENT_H_
#define OPAQUEDB_STORAGE_SEGMENT_H_

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/storage/mapped_file.h"

// A segment is an immutable file of fixed-size records, the on-disk form of one
// column. Storage stores opaque bytes; it does not interpret a record. The
// header is a fixed 64-byte block (so records are 64-byte aligned for SIMD slot
// loads) carrying a magic, a format version, the record stride, the record
// count, and a CRC32 over the records region.
//
// The reader maps the file read-only and validates every header field against
// the real file size before any record is exposed, so a truncated or corrupt
// file is rejected with a status rather than read out of bounds.

namespace opaquedb::storage {

inline constexpr std::uint32_t kSegmentFormatVersion = 1;
inline constexpr std::size_t kSegmentHeaderSize = 64;

// Writes a segment file from records already in memory. Every record must be
// exactly record_bytes long. The file is fsynced before the path is returned,
// so a successful write is durable.
absl::Status WriteSegment(const std::string &path, std::uint32_t record_bytes,
                          const std::vector<std::vector<std::uint8_t>> &records,
                          std::uint32_t *out_crc32);

// A read-only view over a published segment. Records are returned as spans into
// the mapping; they are valid for the lifetime of the reader.
class SegmentReader {
public:
  // Opens and validates a segment. Optionally checks the record stride and
  // count against the values the manifest expects (0 means "do not check").
  static absl::StatusOr<std::unique_ptr<SegmentReader>>
  Open(const std::string &path, std::uint32_t expect_record_bytes,
       std::uint64_t expect_record_count, std::uint32_t expect_crc32);

  std::uint32_t record_bytes() const { return record_bytes_; }
  std::uint64_t record_count() const { return record_count_; }
  std::uint32_t crc32() const { return crc32_; }

  // The record at index, bounds-checked. Fails if index >= record_count.
  absl::StatusOr<std::span<const std::uint8_t>>
  Record(std::uint64_t index) const;

private:
  SegmentReader(MappedFile file, std::uint32_t record_bytes,
                std::uint64_t record_count, std::uint32_t crc32)
      : file_(std::move(file)), record_bytes_(record_bytes),
        record_count_(record_count), crc32_(crc32) {}

  MappedFile file_;
  std::uint32_t record_bytes_;
  std::uint64_t record_count_;
  std::uint32_t crc32_;
};

} // namespace opaquedb::storage

#endif // OPAQUEDB_STORAGE_SEGMENT_H_

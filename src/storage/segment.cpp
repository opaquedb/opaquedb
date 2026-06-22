#include "opaquedb/storage/segment.h"

#include <array>
#include <cstring>
#include <utility>

#include "absl/strings/str_cat.h"
#include "checked.h"
#include "crc32.h"
#include "io_util.h"
#include "little_endian.h"

namespace opaquedb::storage {
namespace {

// 8-byte segment magic. The trailing version byte lets a future format be told
// apart even before the version field is parsed.
constexpr std::array<std::uint8_t, 8> kMagic = {'O', 'P', 'Q', 'S',
                                                'E', 'G', '1', 0};

// Header field offsets within the 64-byte header.
constexpr std::size_t kOffMagic = 0;
constexpr std::size_t kOffVersion = 8;
constexpr std::size_t kOffRecordBytes = 12;
constexpr std::size_t kOffRecordCount = 16;
constexpr std::size_t kOffCrc32 = 24;

} // namespace

absl::Status WriteSegment(const std::string &path, std::uint32_t record_bytes,
                          const std::vector<std::vector<std::uint8_t>> &records,
                          std::uint32_t *out_crc32) {
  if (record_bytes == 0) {
    return absl::InvalidArgumentError("segment: record_bytes is zero");
  }
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (records[i].size() != record_bytes) {
      return absl::InvalidArgumentError(
          absl::StrCat("segment: record ", i, " is ", records[i].size(),
                       " bytes, expected ", record_bytes));
    }
  }

  // Lay records out contiguously, compute the CRC over that region.
  const std::optional<std::uint64_t> region =
      CheckedMul(records.size(), record_bytes);
  if (!region) {
    return absl::OutOfRangeError("segment: records region size overflows");
  }
  const std::optional<std::uint64_t> total =
      CheckedAdd(kSegmentHeaderSize, *region);
  if (!total || *total > SIZE_MAX) {
    return absl::OutOfRangeError("segment: file size overflows");
  }

  std::vector<std::uint8_t> file(static_cast<std::size_t>(*total), 0);
  std::size_t offset = kSegmentHeaderSize;
  for (const std::vector<std::uint8_t> &record : records) {
    std::memcpy(file.data() + offset, record.data(), record.size());
    offset += record.size();
  }

  const std::uint32_t crc = Crc32(std::span<const std::uint8_t>(file).subspan(
      kSegmentHeaderSize, static_cast<std::size_t>(*region)));

  // Build the header in place. Field-by-field, little-endian, no struct cast.
  std::memcpy(file.data() + kOffMagic, kMagic.data(), kMagic.size());
  {
    std::vector<std::uint8_t> tmp;
    AppendU32LE(tmp, kSegmentFormatVersion);
    std::memcpy(file.data() + kOffVersion, tmp.data(), tmp.size());
  }
  {
    std::vector<std::uint8_t> tmp;
    AppendU32LE(tmp, record_bytes);
    std::memcpy(file.data() + kOffRecordBytes, tmp.data(), tmp.size());
  }
  {
    std::vector<std::uint8_t> tmp;
    AppendU64LE(tmp, records.size());
    std::memcpy(file.data() + kOffRecordCount, tmp.data(), tmp.size());
  }
  {
    std::vector<std::uint8_t> tmp;
    AppendU32LE(tmp, crc);
    std::memcpy(file.data() + kOffCrc32, tmp.data(), tmp.size());
  }

  if (absl::Status s = WriteFileDurably(path, file); !s.ok())
    return s;
  if (out_crc32 != nullptr)
    *out_crc32 = crc;
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<SegmentReader>>
SegmentReader::Open(const std::string &path, std::uint32_t expect_record_bytes,
                    std::uint64_t expect_record_count,
                    std::uint32_t expect_crc32) {
  absl::StatusOr<MappedFile> mapped = MappedFile::Open(path);
  if (!mapped.ok())
    return mapped.status();
  std::span<const std::uint8_t> data = mapped->bytes();

  if (data.size() < kSegmentHeaderSize) {
    return absl::DataLossError(
        absl::StrCat("segment '", path, "' shorter than its header"));
  }
  if (std::memcmp(data.data() + kOffMagic, kMagic.data(), kMagic.size()) != 0) {
    return absl::DataLossError(
        absl::StrCat("segment '", path, "' has a bad magic"));
  }

  const std::uint32_t version = *ReadU32LE(data, kOffVersion);
  if (version != kSegmentFormatVersion) {
    return absl::FailedPreconditionError(
        absl::StrCat("segment '", path, "' format version ", version,
                     ", this build supports ", kSegmentFormatVersion));
  }
  const std::uint32_t record_bytes = *ReadU32LE(data, kOffRecordBytes);
  const std::uint64_t record_count = *ReadU64LE(data, kOffRecordCount);
  const std::uint32_t stored_crc = *ReadU32LE(data, kOffCrc32);

  if (record_bytes == 0) {
    return absl::DataLossError(
        absl::StrCat("segment '", path, "' declares record_bytes 0"));
  }

  // The declared geometry must fit inside the real file. This is the check that
  // stops a corrupt or hostile header from driving an out-of-bounds read.
  const std::optional<std::uint64_t> region =
      CheckedMul(record_count, record_bytes);
  if (!region) {
    return absl::DataLossError(
        absl::StrCat("segment '", path, "' geometry overflows"));
  }
  const std::optional<std::uint64_t> need =
      CheckedAdd(kSegmentHeaderSize, *region);
  if (!need || *need > data.size()) {
    return absl::DataLossError(absl::StrCat(
        "segment '", path, "' declares more records than the file holds"));
  }

  const std::uint32_t crc = Crc32(
      data.subspan(kSegmentHeaderSize, static_cast<std::size_t>(*region)));
  if (crc != stored_crc) {
    return absl::DataLossError(
        absl::StrCat("segment '", path, "' failed its CRC check"));
  }

  // Optional cross-checks against the manifest's expectations.
  if (expect_record_bytes != 0 && record_bytes != expect_record_bytes) {
    return absl::FailedPreconditionError(
        absl::StrCat("segment '", path, "' record_bytes ", record_bytes,
                     " does not match manifest ", expect_record_bytes));
  }
  if (expect_record_count != 0 && record_count != expect_record_count) {
    return absl::FailedPreconditionError(
        absl::StrCat("segment '", path, "' record_count ", record_count,
                     " does not match manifest ", expect_record_count));
  }
  if (expect_crc32 != 0 && crc != expect_crc32) {
    return absl::FailedPreconditionError(
        absl::StrCat("segment '", path, "' CRC does not match manifest"));
  }

  return std::unique_ptr<SegmentReader>(
      new SegmentReader(*std::move(mapped), record_bytes, record_count, crc));
}

absl::StatusOr<std::span<const std::uint8_t>>
SegmentReader::Record(std::uint64_t index) const {
  if (index >= record_count_) {
    return absl::OutOfRangeError(absl::StrCat(
        "segment record ", index, " out of range (", record_count_, ")"));
  }
  // index < record_count_ and record_count_ * record_bytes_ was validated to
  // fit the mapping in Open, so this offset is in bounds.
  const std::size_t offset =
      kSegmentHeaderSize + static_cast<std::size_t>(index) * record_bytes_;
  return file_.bytes().subspan(offset, record_bytes_);
}

} // namespace opaquedb::storage

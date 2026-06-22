#include "opaquedb/storage/staging.h"

#include "absl/strings/str_cat.h"
#include "little_endian.h"

namespace opaquedb::storage {

absl::Status SlotGeometry::Validate() const {
  if (slot_count == 0) {
    return absl::InvalidArgumentError("geometry: slot_count is zero");
  }
  if (bytes_per_slot == 0) {
    return absl::InvalidArgumentError("geometry: bytes_per_slot is zero");
  }
  if (record_bytes == 0) {
    return absl::InvalidArgumentError("geometry: record_bytes is zero");
  }
  return absl::OkStatus();
}

std::vector<std::uint8_t> SerializeRow(const Row &row) {
  std::vector<std::uint8_t> out;
  out.reserve(8 + row.match.size() + row.payload.size());
  AppendU32LE(out, static_cast<std::uint32_t>(row.match.size()));
  out.insert(out.end(), row.match.begin(), row.match.end());
  AppendU32LE(out, static_cast<std::uint32_t>(row.payload.size()));
  out.insert(out.end(), row.payload.begin(), row.payload.end());
  return out;
}

absl::StatusOr<Row> ParseRow(std::span<const std::uint8_t> bytes) {
  std::optional<std::uint32_t> match_len = ReadU32LE(bytes, 0);
  if (!match_len) {
    return absl::InvalidArgumentError("row: truncated match length");
  }
  std::size_t offset = 4;
  if (*match_len > bytes.size() - offset) {
    return absl::InvalidArgumentError("row: match length exceeds entry");
  }
  Row row;
  row.match.assign(bytes.begin() + offset, bytes.begin() + offset + *match_len);
  offset += *match_len;

  std::optional<std::uint32_t> payload_len = ReadU32LE(bytes, offset);
  if (!payload_len) {
    return absl::InvalidArgumentError("row: truncated payload length");
  }
  offset += 4;
  if (*payload_len > bytes.size() - offset) {
    return absl::InvalidArgumentError("row: payload length exceeds entry");
  }
  row.payload.assign(bytes.begin() + offset,
                     bytes.begin() + offset + *payload_len);
  offset += *payload_len;

  if (offset != bytes.size()) {
    return absl::InvalidArgumentError("row: trailing bytes after payload");
  }
  return row;
}

absl::Status StagingEpoch::Validate() const {
  if (absl::Status s = schema_.Validate(); !s.ok())
    return s;
  if (key_bits_ == 0 || key_bits_ > 64) {
    return absl::InvalidArgumentError(
        absl::StrCat("staging: key_bits ", key_bits_, " must be in [1, 64]"));
  }
  return geometry_.Validate();
}

absl::Status StagingEpoch::AppendRow(Row row) {
  const std::uint32_t want_match = match_record_bytes();
  if (row.match.size() != want_match) {
    return absl::InvalidArgumentError(
        absl::StrCat("staging: match part is ", row.match.size(),
                     " bytes, expected ", want_match));
  }
  if (row.payload.size() != geometry_.record_bytes) {
    return absl::InvalidArgumentError(
        absl::StrCat("staging: payload is ", row.payload.size(),
                     " bytes, expected ", geometry_.record_bytes));
  }
  rows_.push_back(std::move(row));
  return absl::OkStatus();
}

} // namespace opaquedb::storage

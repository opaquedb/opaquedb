#ifndef OPAQUEDB_CORE_RECORD_CODEC_H_
#define OPAQUEDB_CORE_RECORD_CODEC_H_

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "opaquedb/core/schema.h"

// The typed payload record: how a row's non-key columns are packed into the
// fixed-size record bytes that storage holds and the client reads back. Every
// column except the primary key (kEq) is payload, so a secondary index (kIndex)
// is stored here too and is therefore returnable. The layout follows those
// columns in schema order:
//   int  -> 8 bytes, little-endian signed
//   real -> 8 bytes, little-endian IEEE-754 double
//   text -> 2-byte little-endian length, then that many bytes
// Whatever space is left in the record is zero padding. Encode runs at ingest;
// Decode runs on the client after it decrypts the record. Both are driven by
// the schema so the two sides agree without a separate wire format.

namespace opaquedb::core {

// One column value. The alternatives line up with ColumnType.
using Value = std::variant<std::int64_t, double, std::string>;

// Renders a value for display.
std::string ValueToString(const Value &value);

// The column type that matches a value's active alternative.
ColumnType ColumnTypeOf(const Value &value);

// Parses a single CSV cell into a value of the column's type.
absl::StatusOr<Value> ParseValue(ColumnType type, std::string_view text);

// Packs the payload values (the schema's RAW columns, in order) into a record
// of exactly record_bytes. Rejects a record that does not fit.
absl::StatusOr<std::vector<std::uint8_t>>
EncodeRecord(const Schema &schema, const std::vector<Value> &payload,
             std::uint32_t record_bytes);

// Unpacks a record into the payload column values, in schema order. Every
// length is checked against the available bytes before it is read.
absl::StatusOr<std::vector<Value>>
DecodeRecord(const Schema &schema, std::span<const std::uint8_t> record);

// Packs the match record: one key per searchable column (kEq and every kIndex)
// in schema order, each EncodeKeyValue'd then packed into ceil(key_bits/8)
// bytes. key_value is the primary key column's value; payload holds the
// remaining columns' values in schema order (the same vector EncodeRecord
// takes), from which the index columns' values are read. The engine slices one
// sub-key out of this record at query time by the column's SearchableRank.
absl::StatusOr<std::vector<std::uint8_t>>
EncodeMatchRecord(const Schema &schema, const Value &key_value,
                  const std::vector<Value> &payload, std::uint32_t key_bits);

} // namespace opaquedb::core

#endif // OPAQUEDB_CORE_RECORD_CODEC_H_

#include "opaquedb/storage/epoch_manifest.h"

#include <exception>
#include <vector>

#include "absl/strings/str_cat.h"
#include "nlohmann/json.hpp"

namespace opaquedb::storage {
namespace {

using nlohmann::json;

// Typed, checked getters. Each returns a status rather than throwing or
// defaulting, so a missing or wrong-typed field is a clear parse error.
absl::Status RequireObject(const json &j, const char *what) {
  if (!j.is_object()) {
    return absl::InvalidArgumentError(absl::StrCat(what, " is not an object"));
  }
  return absl::OkStatus();
}

template <typename T>
absl::StatusOr<T> GetNumber(const json &j, const char *key) {
  if (!j.contains(key)) {
    return absl::InvalidArgumentError(
        absl::StrCat("missing field '", key, "'"));
  }
  const json &v = j.at(key);
  if (!v.is_number_unsigned() && !v.is_number_integer()) {
    return absl::InvalidArgumentError(
        absl::StrCat("field '", key, "' is not an integer"));
  }
  if (v.is_number_integer() && v.get<long long>() < 0) {
    return absl::InvalidArgumentError(
        absl::StrCat("field '", key, "' is negative"));
  }
  return v.get<T>();
}

absl::StatusOr<std::string> GetString(const json &j, const char *key) {
  if (!j.contains(key) || !j.at(key).is_string()) {
    return absl::InvalidArgumentError(
        absl::StrCat("missing or non-string field '", key, "'"));
  }
  return j.at(key).get<std::string>();
}

absl::StatusOr<SegmentDescriptor> ParseSegment(const json &j,
                                               const char *what) {
  if (absl::Status s = RequireObject(j, what); !s.ok())
    return s;
  SegmentDescriptor seg;
  auto file = GetString(j, "file");
  if (!file.ok())
    return file.status();
  seg.file = *file;
  auto rb = GetNumber<std::uint32_t>(j, "record_bytes");
  if (!rb.ok())
    return rb.status();
  seg.record_bytes = *rb;
  auto rc = GetNumber<std::uint64_t>(j, "record_count");
  if (!rc.ok())
    return rc.status();
  seg.record_count = *rc;
  auto crc = GetNumber<std::uint32_t>(j, "crc32");
  if (!crc.ok())
    return crc.status();
  seg.crc32 = *crc;
  return seg;
}

json SegmentToJson(const SegmentDescriptor &seg) {
  return json{{"file", seg.file},
              {"record_bytes", seg.record_bytes},
              {"record_count", seg.record_count},
              {"crc32", seg.crc32}};
}

} // namespace

namespace {

// Segment file names in the manifest must be plain names within the epoch
// directory. Rejecting separators and parent references stops a crafted
// manifest from steering a read outside the epoch directory (path traversal).
bool IsSafeBasename(std::string_view name) {
  if (name.empty() || name == "." || name == "..")
    return false;
  return name.find('/') == std::string_view::npos &&
         name.find('\\') == std::string_view::npos;
}

} // namespace

absl::Status EpochManifest::Validate() const {
  if (format_version != kManifestFormatVersion) {
    return absl::FailedPreconditionError(
        absl::StrCat("manifest format version ", format_version,
                     ", this build supports ", kManifestFormatVersion));
  }
  if (absl::Status s = schema.Validate(); !s.ok())
    return s;
  if (key_bits == 0 || key_bits > 64) {
    return absl::InvalidArgumentError(
        absl::StrCat("manifest key_bits invalid: ", key_bits));
  }
  if (absl::Status s = geometry.Validate(); !s.ok())
    return s;
  if (match_segment.record_count != payload_segment.record_count) {
    return absl::InvalidArgumentError(
        absl::StrCat("manifest match rows ", match_segment.record_count,
                     " != payload rows ", payload_segment.record_count));
  }
  if (match_segment.record_bytes != MatchRecordBytes(key_bits)) {
    return absl::InvalidArgumentError(
        "manifest match segment stride does not match key_bits");
  }
  if (payload_segment.record_bytes != geometry.record_bytes) {
    return absl::InvalidArgumentError(
        "manifest payload segment stride does not match the geometry");
  }
  if (!IsSafeBasename(match_segment.file) ||
      !IsSafeBasename(payload_segment.file)) {
    return absl::InvalidArgumentError(
        "manifest segment file name is not a safe basename");
  }
  return absl::OkStatus();
}

std::string SerializeManifest(const EpochManifest &manifest) {
  json columns = json::array();
  for (const core::Column &col : manifest.schema.columns()) {
    columns.push_back(json{{"name", col.name},
                           {"encoding", core::ToString(col.encoding)},
                           {"type", core::ToString(col.type)}});
  }
  json doc = {
      {"format_version", manifest.format_version},
      {"epoch_version", manifest.epoch_version},
      {"schema",
       json{{"table", manifest.schema.table()}, {"columns", columns}}},
      {"key_bits", manifest.key_bits},
      {"geometry", json{{"slot_count", manifest.geometry.slot_count},
                        {"bytes_per_slot", manifest.geometry.bytes_per_slot},
                        {"record_bytes", manifest.geometry.record_bytes}}},
      {"match_segment", SegmentToJson(manifest.match_segment)},
      {"payload_segment", SegmentToJson(manifest.payload_segment)},
  };
  return doc.dump(2);
}

absl::StatusOr<EpochManifest> ParseManifest(std::string_view json_text) {
  json doc = json::parse(json_text, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded()) {
    return absl::InvalidArgumentError("manifest is not valid JSON");
  }
  if (absl::Status s = RequireObject(doc, "manifest"); !s.ok())
    return s;

  EpochManifest m;
  auto fv = GetNumber<std::uint32_t>(doc, "format_version");
  if (!fv.ok())
    return fv.status();
  m.format_version = *fv;
  // Refuse an unknown version before reading the rest; later fields may differ.
  if (m.format_version != kManifestFormatVersion) {
    return absl::FailedPreconditionError(
        absl::StrCat("manifest format version ", m.format_version,
                     ", this build supports ", kManifestFormatVersion));
  }
  auto ev = GetNumber<std::uint64_t>(doc, "epoch_version");
  if (!ev.ok())
    return ev.status();
  m.epoch_version = *ev;

  if (!doc.contains("schema")) {
    return absl::InvalidArgumentError("manifest missing 'schema'");
  }
  const json &schema_json = doc.at("schema");
  if (absl::Status s = RequireObject(schema_json, "schema"); !s.ok())
    return s;
  auto table = GetString(schema_json, "table");
  if (!table.ok())
    return table.status();
  if (!schema_json.contains("columns") ||
      !schema_json.at("columns").is_array()) {
    return absl::InvalidArgumentError("schema missing 'columns' array");
  }
  std::vector<core::Column> columns;
  for (const json &cj : schema_json.at("columns")) {
    if (absl::Status s = RequireObject(cj, "column"); !s.ok())
      return s;
    auto name = GetString(cj, "name");
    if (!name.ok())
      return name.status();
    auto enc_text = GetString(cj, "encoding");
    if (!enc_text.ok())
      return enc_text.status();
    auto enc = core::ParseColumnEncoding(*enc_text);
    if (!enc.ok())
      return enc.status();
    // Older manifests carry no type; default to text so they still parse.
    core::ColumnType type = core::ColumnType::kText;
    if (cj.contains("type")) {
      auto type_text = GetString(cj, "type");
      if (!type_text.ok())
        return type_text.status();
      auto parsed = core::ParseColumnType(*type_text);
      if (!parsed.ok())
        return parsed.status();
      type = *parsed;
    }
    columns.push_back(core::Column{*name, *enc, type});
  }
  m.schema = core::Schema(*table, std::move(columns));

  auto kb = GetNumber<std::uint32_t>(doc, "key_bits");
  if (!kb.ok())
    return kb.status();
  m.key_bits = *kb;

  if (!doc.contains("geometry")) {
    return absl::InvalidArgumentError("manifest missing 'geometry'");
  }
  const json &geo = doc.at("geometry");
  if (absl::Status s = RequireObject(geo, "geometry"); !s.ok())
    return s;
  auto sc = GetNumber<std::uint32_t>(geo, "slot_count");
  if (!sc.ok())
    return sc.status();
  auto bps = GetNumber<std::uint32_t>(geo, "bytes_per_slot");
  if (!bps.ok())
    return bps.status();
  auto rb = GetNumber<std::uint32_t>(geo, "record_bytes");
  if (!rb.ok())
    return rb.status();
  m.geometry = SlotGeometry{*sc, *bps, *rb};

  if (!doc.contains("match_segment") || !doc.contains("payload_segment")) {
    return absl::InvalidArgumentError("manifest missing a segment descriptor");
  }
  auto match = ParseSegment(doc.at("match_segment"), "match_segment");
  if (!match.ok())
    return match.status();
  m.match_segment = *match;
  auto payload = ParseSegment(doc.at("payload_segment"), "payload_segment");
  if (!payload.ok())
    return payload.status();
  m.payload_segment = *payload;

  if (absl::Status s = m.Validate(); !s.ok())
    return s;
  return m;
}

} // namespace opaquedb::storage

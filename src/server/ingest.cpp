#include "opaquedb/server/ingest.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "opaquedb/core/wire.h"

namespace opaquedb::server {
namespace {

storage::SlotGeometry GeometryFrom(const config::Config &config) {
  storage::SlotGeometry geom;
  geom.slot_count = config.crypto.poly_modulus_degree;
  geom.bytes_per_slot = config.crypto.BytesPerSlot();
  geom.record_bytes = config.storage.record_bytes;
  return geom;
}

// The single EQ column is the match key.
const core::Column *MatchKey(const core::Schema &schema) {
  for (const core::Column &col : schema.columns()) {
    if (col.encoding == core::ColumnEncoding::kEq)
      return &col;
  }
  return nullptr;
}

} // namespace

absl::StatusOr<storage::StagingEpoch>
BuildStagingFromRows(const config::Config &config, const core::Schema &schema,
                     const std::vector<IngestRow> &rows) {
  if (absl::Status s = schema.Validate(); !s.ok())
    return s;

  const core::Column *key = MatchKey(schema);
  if (key == nullptr)
    return absl::InvalidArgumentError("schema has no match key column");

  const std::uint32_t key_bits = config.crypto.key_bits;
  storage::StagingEpoch staging(schema, key_bits, GeometryFrom(config));
  if (absl::Status s = staging.Validate(); !s.ok())
    return s;

  for (std::size_t i = 0; i < rows.size(); ++i) {
    absl::StatusOr<std::vector<std::uint8_t>> match =
        core::EncodeMatchRecord(schema, rows[i].key, rows[i].payload, key_bits);
    if (!match.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("row ", i, ": ", match.status().message()));
    }
    absl::StatusOr<std::vector<std::uint8_t>> payload = core::EncodeRecord(
        schema, rows[i].payload, config.storage.record_bytes);
    if (!payload.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("row ", i, ": ", payload.status().message()));
    }
    storage::Row row;
    row.match = *std::move(match);
    row.payload = *std::move(payload);
    if (absl::Status s = staging.AppendRow(std::move(row)); !s.ok())
      return s;
  }
  return staging;
}

absl::StatusOr<std::vector<IngestRow>>
ParseCsvRows(const core::Schema &schema, const std::string &csv_path) {
  if (absl::Status s = schema.Validate(); !s.ok())
    return s;
  const core::Column *key = MatchKey(schema);
  if (key == nullptr)
    return absl::InvalidArgumentError("schema has no match key column");

  std::ifstream in(csv_path);
  if (!in) {
    return absl::NotFoundError(
        absl::StrCat("cannot open CSV '", csv_path, "'"));
  }

  std::string line;
  // The header names the columns; map each schema column to its CSV position so
  // the file's column order does not have to match the schema's.
  if (!std::getline(in, line)) {
    return absl::InvalidArgumentError("CSV is empty; expected a header row");
  }
  std::vector<std::string> header =
      absl::StrSplit(absl::StripAsciiWhitespace(line), ',');
  for (std::string &h : header)
    h = std::string(absl::StripAsciiWhitespace(h));

  auto column_index =
      [&](const std::string &name) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < header.size(); ++i) {
      if (header[i] == name)
        return i;
    }
    return std::nullopt;
  };

  std::optional<std::size_t> key_pos = column_index(key->name);
  if (!key_pos) {
    return absl::InvalidArgumentError(
        absl::StrCat("CSV header has no key column '", key->name, "'"));
  }
  for (const core::Column &col : schema.columns()) {
    if (!column_index(col.name)) {
      return absl::InvalidArgumentError(
          absl::StrCat("CSV header has no column '", col.name, "'"));
    }
  }

  std::vector<IngestRow> rows;
  std::size_t line_no = 1;
  while (std::getline(in, line)) {
    ++line_no;
    if (absl::StripAsciiWhitespace(line).empty())
      continue;
    std::vector<std::string> cells = absl::StrSplit(line, ',');
    if (cells.size() != header.size()) {
      return absl::InvalidArgumentError(
          absl::StrCat("line ", line_no, ": expected ", header.size(),
                       " columns, got ", cells.size()));
    }

    IngestRow row;
    std::string_view key_cell = absl::StripAsciiWhitespace(cells[*key_pos]);
    absl::StatusOr<core::Value> key_value =
        core::ParseValue(key->type, key_cell);
    if (!key_value.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("line ", line_no, ", key column '", key->name,
                       "': ", key_value.status().message()));
    }
    row.key = *std::move(key_value);
    for (const core::Column &col : schema.columns()) {
      if (col.encoding == core::ColumnEncoding::kEq)
        continue;
      const std::size_t pos = *column_index(col.name);
      absl::StatusOr<core::Value> value =
          core::ParseValue(col.type, absl::StripAsciiWhitespace(cells[pos]));
      if (!value.ok()) {
        return absl::InvalidArgumentError(
            absl::StrCat("line ", line_no, ", column '", col.name,
                         "': ", value.status().message()));
      }
      row.payload.push_back(*std::move(value));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

absl::StatusOr<storage::StagingEpoch>
BuildStagingFromCsv(const config::Config &config, const core::Schema &schema,
                    const std::string &csv_path) {
  absl::StatusOr<std::vector<IngestRow>> rows = ParseCsvRows(schema, csv_path);
  if (!rows.ok())
    return rows.status();
  return BuildStagingFromRows(config, schema, *rows);
}

absl::StatusOr<InsertResult>
InsertRowAndPublish(storage::EpochRepository &repo,
                    const std::vector<std::string> &cells) {
  // Hold the table's write lock for the whole read-modify-publish: we copy the
  // current epoch's rows and append one, so a concurrent insert must not slip a
  // publish in between (it would be silently dropped when we publish the next
  // version built only from rows we saw).
  std::lock_guard<std::mutex> write_lock(repo.write_mutex());

  // The table must already exist; we reuse its current schema and geometry so
  // an insert never changes the layout. A fresh table is created by load.
  absl::StatusOr<std::unique_ptr<storage::EpochReader>> reader =
      repo.OpenCurrent();
  if (!reader.ok())
    return reader.status();
  const storage::EpochManifest &manifest = (*reader)->manifest();
  const core::Schema &schema = manifest.schema;
  const std::uint32_t key_bits = manifest.key_bits;

  const core::Column *key = MatchKey(schema);
  if (key == nullptr)
    return absl::InvalidArgumentError("schema has no match key column");

  const std::vector<core::Column> &cols = schema.columns();
  if (cells.size() != cols.size()) {
    return absl::InvalidArgumentError(
        absl::StrCat("insert expects ", cols.size(),
                     " values in column order, got ", cells.size()));
  }

  // Parse the cells in column order: the key column becomes the match key, the
  // rest become payload values (the same order EncodeRecord expects, since it
  // skips the key column).
  core::Value key_value;
  std::vector<core::Value> payload;
  payload.reserve(cols.size());
  for (std::size_t i = 0; i < cols.size(); ++i) {
    absl::StatusOr<core::Value> v =
        core::ParseValue(cols[i].type, absl::StripAsciiWhitespace(cells[i]));
    if (!v.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("column '", cols[i].name, "': ", v.status().message()));
    }
    if (cols[i].encoding == core::ColumnEncoding::kEq)
      key_value = *std::move(v);
    else
      payload.push_back(*std::move(v));
  }

  // Rebuild the epoch: copy every existing row as-is (raw match and payload
  // bytes, no decode), then append the new row.
  storage::StagingEpoch staging(schema, key_bits, manifest.geometry);
  if (absl::Status s = staging.Validate(); !s.ok())
    return s;
  const std::uint64_t existing = (*reader)->row_count();
  for (std::uint64_t r = 0; r < existing; ++r) {
    absl::StatusOr<std::span<const std::uint8_t>> m = (*reader)->MatchRecord(r);
    if (!m.ok())
      return m.status();
    absl::StatusOr<std::span<const std::uint8_t>> p =
        (*reader)->PayloadRecord(r);
    if (!p.ok())
      return p.status();
    storage::Row row;
    row.match.assign(m->begin(), m->end());
    row.payload.assign(p->begin(), p->end());
    if (absl::Status s = staging.AppendRow(std::move(row)); !s.ok())
      return s;
  }

  absl::StatusOr<std::vector<std::uint8_t>> match =
      core::EncodeMatchRecord(schema, key_value, payload, key_bits);
  if (!match.ok())
    return match.status();
  absl::StatusOr<std::vector<std::uint8_t>> rec =
      core::EncodeRecord(schema, payload, manifest.geometry.record_bytes);
  if (!rec.ok())
    return rec.status();
  storage::Row new_row;
  new_row.match = *std::move(match);
  new_row.payload = *std::move(rec);
  if (absl::Status s = staging.AppendRow(std::move(new_row)); !s.ok())
    return s;

  absl::StatusOr<std::uint64_t> current = repo.CurrentVersion();
  const std::uint64_t next = current.ok() ? *current + 1 : 1;
  if (absl::Status s = repo.Publish(staging, next); !s.ok())
    return s;

  // Each insert republishes the whole table as a new epoch, so without pruning
  // the superseded epochs pile up without bound (disk grows quadratically over
  // a row-by-row load). Keep a small rollback window and drop the rest. Still
  // under the write lock, so this cannot race another writer's publish; a prune
  // failure must not fail an insert that already committed, so it is best
  // effort.
  constexpr std::uint64_t kInsertEpochRetention = 8;
  (void)repo.Prune(kInsertEpochRetention);
  return InsertResult{next, existing + 1};
}

} // namespace opaquedb::server

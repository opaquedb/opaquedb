#ifndef OPAQUEDB_SERVER_INGEST_H_
#define OPAQUEDB_SERVER_INGEST_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/storage/repository.h"
#include "opaquedb/storage/staging.h"

// Ingest turns rows into a staging epoch under a user-declared schema. The
// schema names the match key (an integer column encoded as a codeword) and the
// payload columns (typed values packed into the record). There is one ingest
// path; the load command and the admin Load RPC both feed it.

namespace opaquedb::server {

// A single row: the typed match key value and the payload column values, in the
// schema's column order (RAW columns only). The key is mapped to a codeword at
// staging time using the schema's key type.
struct IngestRow {
  core::Value key;
  std::vector<core::Value> payload;
};

// Builds a staging epoch from rows in memory using the FHE and storage
// parameters from config. The key must fit the constant-weight code space; the
// encoded payload must fit the record size. This is the one ingest path.
absl::StatusOr<storage::StagingEpoch>
BuildStagingFromRows(const config::Config &config, const core::Schema &schema,
                     const std::vector<IngestRow> &rows);

// Reads a CSV whose first line is a header naming the columns into rows.
// Columns are matched to the schema by name, so order in the file does not
// matter. Every cell, key and payload, is parsed by its column type. The load
// command filters these rows by shard before building.
absl::StatusOr<std::vector<IngestRow>>
ParseCsvRows(const core::Schema &schema, const std::string &csv_path);

// Reads a CSV (see ParseCsvRows) and builds a staging epoch from every row.
absl::StatusOr<storage::StagingEpoch>
BuildStagingFromCsv(const config::Config &config, const core::Schema &schema,
                    const std::string &csv_path);

// The outcome of an insert: the new epoch version and the table's total row
// count after the row was appended.
struct InsertResult {
  std::uint64_t epoch_version = 0;
  std::uint64_t row_count = 0;
};

// Appends one row to a table's current epoch and publishes a new immutable
// epoch. cells are the column values as text in the schema's declared column
// order (every column, including the match key). The new epoch reuses the
// current one's schema, key_bits, and geometry so the layout stays consistent.
// The table must already exist (have a current epoch); load it first. Epochs are
// immutable, so this rewrites the existing rows plus the new one into a new
// version. This is the one append path; the gRPC Insert RPC calls it.
absl::StatusOr<InsertResult> InsertRowAndPublish(
    storage::EpochRepository &repo, const std::vector<std::string> &cells);

} // namespace opaquedb::server

#endif // OPAQUEDB_SERVER_INGEST_H_

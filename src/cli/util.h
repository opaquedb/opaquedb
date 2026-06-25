#ifndef OPAQUEDB_CLI_UTIL_H_
#define OPAQUEDB_CLI_UTIL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "opaquedb/cli/command.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/schema.h"

// Helpers shared by every command, so config loading, logging setup, and file
// reading live in one place instead of being repeated per command.

namespace opaquedb::cli {

// Resolves config from the global options and initializes logging from it, so
// every command logs through the one configured logger. Returns the config or
// the first error.
absl::StatusOr<config::Config> LoadConfig(const GlobalOptions &globals,
                                          bool file_optional = true);

// Reads a whole file into a string.
absl::StatusOr<std::string> ReadFile(const std::string &path);

// Trims trailing NUL padding from a fixed-size record for raw display.
std::string RenderRaw(const std::vector<std::uint8_t> &record);

// Renders one record as "name=value" pairs. If projection is non-empty, only
// those columns are shown, in projection order; otherwise every payload column
// is shown. A projected column that is not a payload column (the key, or
// unknown) is skipped.
std::string RenderWithSchema(const core::Schema &schema,
                             const std::vector<std::uint8_t> &record,
                             const std::vector<std::string> &projection = {});

} // namespace opaquedb::cli

#endif // OPAQUEDB_CLI_UTIL_H_

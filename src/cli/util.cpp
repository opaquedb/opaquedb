#include "util.h"

#include <fstream>
#include <sstream>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "opaquedb/config/loader.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/log/log.h"

namespace opaquedb::cli {

absl::StatusOr<config::Config> LoadConfig(const GlobalOptions &globals,
                                          bool file_optional) {
  absl::StatusOr<config::LoadOptions> opts =
      globals.ToLoadOptions(file_optional);
  if (!opts.ok())
    return opts.status();
  absl::StatusOr<config::Config> config = config::Load(*opts);
  if (!config.ok())
    return config.status();
  if (absl::Status s = log::Init(config->logging); !s.ok())
    return s;
  return config;
}

absl::StatusOr<std::string> ReadFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return absl::NotFoundError(absl::StrCat("cannot read '", path, "'"));
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string RenderRaw(const std::vector<std::uint8_t> &record) {
  std::size_t end = record.size();
  while (end > 0 && record[end - 1] == 0)
    --end;
  return std::string(record.begin(), record.begin() + end);
}

std::string RenderWithSchema(const core::Schema &schema,
                             const std::vector<std::uint8_t> &record,
                             const std::vector<std::string> &projection) {
  absl::StatusOr<std::vector<core::Value>> values =
      core::DecodeRecord(schema, record);
  if (!values.ok())
    return absl::StrCat("(decode error: ", values.status().message(), ")");
  // DecodeRecord yields payload (non-key) columns in schema order. Map each
  // payload column name to its value so the projection can pick by name.
  std::vector<std::pair<std::string, std::string>> payload;
  std::size_t i = 0;
  for (const core::Column &col : schema.columns()) {
    if (col.encoding == core::ColumnEncoding::kEq)
      continue;
    if (i >= values->size())
      break;
    payload.emplace_back(col.name, core::ValueToString((*values)[i++]));
  }

  std::vector<std::string> parts;
  if (projection.empty()) {
    for (const auto &[name, value] : payload)
      parts.push_back(absl::StrCat(name, "=", value));
  } else {
    for (const std::string &want : projection) {
      for (const auto &[name, value] : payload) {
        if (name == want) {
          parts.push_back(absl::StrCat(name, "=", value));
          break;
        }
      }
    }
  }
  return absl::StrJoin(parts, " ");
}

} // namespace opaquedb::cli

#ifndef OPAQUEDB_CONFIG_LOADER_H_
#define OPAQUEDB_CONFIG_LOADER_H_

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "opaquedb/config/config.h"

// The layered configuration loader. Resolution order, later overrides earlier:
//
//   1. built-in defaults (the Config struct's member initializers)
//   2. the TOML config file
//   3. environment variables (OPAQUEDB_<SECTION>_<KEY>)
//   4. command-line flags
//
// The whole config is validated on load and the loader fails fast with a clear
// absl::Status if anything is wrong. Nothing starts on invalid config.
//
// A single key/value setter (ApplyKeyValue) is the one place that maps a dotted
// key such as "server.listen" to a field and parses its string form. The env
// and flag layers both go through it, so there is one parser, not three.

namespace opaquedb::config {

// Looks up an environment variable, returning nullopt if unset. Injected so
// tests can supply a fake environment with no real getenv.
using EnvLookup =
    std::function<std::optional<std::string>(std::string_view name)>;

// Reads the process environment with std::getenv.
EnvLookup SystemEnv();

// The default config path. Overridable with --config or OPAQUEDB_CONFIG.
inline constexpr std::string_view kDefaultConfigPath =
    "/etc/opaquedb/opaquedb.toml";

// The commented default config, written by `config init`. This is the single
// source for both the subcommand output and packaging/opaquedb.toml.example.
std::string_view DefaultConfigToml();

// Options controlling a load. The config path is resolved by ResolveConfigPath
// before calling Load so the precedence of --config over OPAQUEDB_CONFIG over
// the default is explicit and testable.
struct LoadOptions {
  // The TOML file to read. If empty, no file layer is applied (defaults only,
  // then env and flags). If set but missing, Load fails unless
  // file_is_optional is true.
  std::string config_path;
  bool file_is_optional = false;

  // Environment source. Defaults to the real environment.
  EnvLookup env = SystemEnv();

  // Flag overrides as dotted keys, e.g. {"server.listen", "0.0.0.0:6000"}.
  // Applied last so flags win. Built from CLI options by the cli layer.
  std::map<std::string, std::string> flag_overrides;
};

// Resolves the effective config path from a --config flag value and the
// environment, falling back to the default. Empty flag means "not given".
std::string ResolveConfigPath(std::string_view config_flag,
                              const EnvLookup &env);

// Builds defaults, applies file, env, and flags in order, then validates.
absl::StatusOr<Config> Load(const LoadOptions &options);

// Applies one dotted key/value to a Config, parsing the value to the field's
// type. Unknown keys are an error. This is the single key parser used by the
// env and flag layers.
absl::Status ApplyKeyValue(Config &config, std::string_view dotted_key,
                           std::string_view value);

// Validates a fully resolved Config. Returns the first problem found with a
// clear message. Checked here so neither the loader nor callers duplicate it.
absl::Status Validate(const Config &config);

// Serializes a Config to TOML text for `config print`. The output parses back
// to an equivalent Config.
std::string ToToml(const Config &config);

// Applies only the defaults and the file layer, then validates. Used by
// `config validate`, which checks a file in isolation.
absl::StatusOr<Config> LoadFileOnly(std::string_view config_path);

} // namespace opaquedb::config

#endif // OPAQUEDB_CONFIG_LOADER_H_

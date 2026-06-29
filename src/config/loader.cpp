#include "opaquedb/config/loader.h"

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "toml++/toml.hpp"

namespace opaquedb::config {
namespace {

absl::StatusOr<std::uint64_t> ParseU64(std::string_view key,
                                       std::string_view value) {
  std::uint64_t out = 0;
  if (!absl::SimpleAtoi(value, &out)) {
    return absl::InvalidArgumentError(
        absl::StrCat(key, ": expected an unsigned integer, got '", value, "'"));
  }
  return out;
}

absl::StatusOr<std::uint32_t> ParseU32(std::string_view key,
                                       std::string_view value) {
  auto v = ParseU64(key, value);
  if (!v.ok())
    return v.status();
  if (*v > UINT32_MAX) {
    return absl::InvalidArgumentError(
        absl::StrCat(key, ": value ", *v, " is out of range"));
  }
  return static_cast<std::uint32_t>(*v);
}

absl::StatusOr<bool> ParseBool(std::string_view key, std::string_view value) {
  std::string v = absl::AsciiStrToLower(value);
  if (v == "true" || v == "1" || v == "yes" || v == "on")
    return true;
  if (v == "false" || v == "0" || v == "no" || v == "off")
    return false;
  return absl::InvalidArgumentError(
      absl::StrCat(key, ": expected a boolean, got '", value, "'"));
}

absl::StatusOr<std::vector<int>> ParseIntList(std::string_view key,
                                              std::string_view value) {
  std::vector<int> out;
  std::string trimmed(absl::StripAsciiWhitespace(value));
  if (trimmed.empty())
    return out;
  for (std::string_view part : absl::StrSplit(trimmed, ',')) {
    std::string_view item = absl::StripAsciiWhitespace(part);
    int n = 0;
    if (!absl::SimpleAtoi(item, &n)) {
      return absl::InvalidArgumentError(absl::StrCat(
          key, ": expected a comma-separated integer list, got '", value, "'"));
    }
    out.push_back(n);
  }
  return out;
}

std::vector<std::string> ParseStringList(std::string_view value) {
  std::vector<std::string> out;
  std::string trimmed(absl::StripAsciiWhitespace(value));
  if (trimmed.empty())
    return out;
  for (std::string_view part : absl::StrSplit(trimmed, ',')) {
    out.emplace_back(absl::StripAsciiWhitespace(part));
  }
  return out;
}

absl::StatusOr<AuthMode> ParseAuthMode(std::string_view value) {
  if (value == "token")
    return AuthMode::kToken;
  if (value == "mtls")
    return AuthMode::kMtls;
  if (value == "none")
    return AuthMode::kNone;
  return absl::InvalidArgumentError(
      absl::StrCat("auth.mode: expected token|mtls|none, got '", value, "'"));
}

absl::StatusOr<LogFormat> ParseLogFormat(std::string_view value) {
  if (value == "json")
    return LogFormat::kJson;
  if (value == "text")
    return LogFormat::kText;
  return absl::InvalidArgumentError(
      absl::StrCat("logging.format: expected json|text, got '", value, "'"));
}

// Converts a TOML node to the string form ApplyKeyValue expects. Arrays join
// with commas so the one key parser handles file, env, and flag values alike.
std::string TomlNodeToString(const toml::node &node) {
  if (const auto *arr = node.as_array()) {
    std::vector<std::string> parts;
    for (const auto &el : *arr) {
      parts.push_back(TomlNodeToString(el));
    }
    return absl::StrJoin(parts, ",");
  }
  if (const auto *s = node.as_string())
    return s->get();
  if (const auto *i = node.as_integer())
    return absl::StrCat(i->get());
  if (const auto *b = node.as_boolean())
    return b->get() ? "true" : "false";
  if (const auto *f = node.as_floating_point())
    return absl::StrCat(f->get());
  return "";
}

// The full set of dotted keys, used by the TOML reader to know which keys to
// pull from the file. The key strings live here once and ApplyKeyValue parses
// them.
constexpr std::string_view kKeys[] = {
    "node.id",
    "node.data_dir",
    "cluster.enabled",
    "cluster.etcd_endpoints",
    "cluster.leader_key",
    "cluster.etcd_username",
    "cluster.etcd_password",
    "cluster.etcd_ca_cert",
    "cluster.etcd_client_cert",
    "cluster.etcd_client_key",
    "cluster.etcd_tls_name",
    "cluster.listen",
    "cluster.advertise",
    "cluster.tls_cert",
    "cluster.tls_key",
    "cluster.ca_cert",
    "cluster.allow_insecure",
    "server.listen",
    "server.advertise",
    "server.max_message_bytes",
    "server.tls_cert",
    "server.tls_key",
    "server.rate_limit_per_second",
    "server.rate_limit_burst",
    "client.ca_cert",
    "client.tls_cert",
    "client.tls_key",
    "client.server_name",
    "client.allow_insecure",
    "crypto.poly_modulus_degree",
    "crypto.plain_modulus_bits",
    "crypto.coeff_modulus_bits",
    "crypto.key_bits",
    "crypto.result_buckets",
    "storage.record_bytes",
    "storage.epoch_dir",
    "auth.mode",
    "auth.enable_insecure",
    "auth.token_file",
    "auth.ca_cert",
    "auth.admin_identities",
    "blobstore.path",
    "metrics.listen",
    "logging.level",
    "logging.format",
    "logging.file",
};

// Maps a dotted key to the OPAQUEDB_ env var name: uppercase, dots to
// underscores. So server.listen maps to OPAQUEDB_SERVER_LISTEN.
std::string EnvNameForKey(std::string_view dotted_key) {
  std::string name = "OPAQUEDB_";
  for (char c : dotted_key) {
    name.push_back(c == '.' ? '_' : absl::ascii_toupper(c));
  }
  return name;
}

absl::Status ApplyTomlFile(Config &config, std::string_view path,
                           bool optional) {
  toml::table table;
  try {
    table = toml::parse_file(path);
  } catch (const toml::parse_error &err) {
    if (optional)
      return absl::OkStatus();
    return absl::InvalidArgumentError(absl::StrCat(
        "failed to parse config file '", path, "': ", err.description()));
  }
  for (std::string_view key : kKeys) {
    std::pair<std::string_view, std::string_view> parts =
        absl::StrSplit(key, absl::MaxSplits('.', 1));
    const auto node = table[parts.first][parts.second];
    if (!node)
      continue;
    if (absl::Status s =
            ApplyKeyValue(config, key, TomlNodeToString(*node.node()));
        !s.ok()) {
      return s;
    }
  }
  return absl::OkStatus();
}

absl::Status ApplyEnv(Config &config, const EnvLookup &env) {
  for (std::string_view key : kKeys) {
    std::optional<std::string> value = env(EnvNameForKey(key));
    if (!value)
      continue;
    if (absl::Status s = ApplyKeyValue(config, key, *value); !s.ok()) {
      return s;
    }
  }
  return absl::OkStatus();
}

bool IsPowerOfTwo(std::uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

} // namespace

EnvLookup SystemEnv() {
  return [](std::string_view name) -> std::optional<std::string> {
    const char *value = std::getenv(std::string(name).c_str());
    if (value == nullptr)
      return std::nullopt;
    return std::string(value);
  };
}

absl::Status ApplyKeyValue(Config &config, std::string_view key,
                           std::string_view value) {
  if (key == "node.id") {
    config.node.id = std::string(value);
  } else if (key == "node.data_dir") {
    config.node.data_dir = std::string(value);
  } else if (key == "cluster.enabled") {
    auto v = ParseBool(key, value);
    if (!v.ok())
      return v.status();
    config.cluster.enabled = *v;
  } else if (key == "cluster.etcd_endpoints") {
    config.cluster.etcd_endpoints = ParseStringList(value);
  } else if (key == "cluster.leader_key") {
    config.cluster.leader_key = std::string(value);
  } else if (key == "cluster.etcd_username") {
    config.cluster.etcd_username = std::string(value);
  } else if (key == "cluster.etcd_password") {
    config.cluster.etcd_password = std::string(value);
  } else if (key == "cluster.etcd_ca_cert") {
    config.cluster.etcd_ca_cert = std::string(value);
  } else if (key == "cluster.etcd_client_cert") {
    config.cluster.etcd_client_cert = std::string(value);
  } else if (key == "cluster.etcd_client_key") {
    config.cluster.etcd_client_key = std::string(value);
  } else if (key == "cluster.etcd_tls_name") {
    config.cluster.etcd_tls_name = std::string(value);
  } else if (key == "cluster.listen") {
    config.cluster.listen = std::string(value);
  } else if (key == "cluster.advertise") {
    config.cluster.advertise = std::string(value);
  } else if (key == "cluster.tls_cert") {
    config.cluster.tls_cert = std::string(value);
  } else if (key == "cluster.tls_key") {
    config.cluster.tls_key = std::string(value);
  } else if (key == "cluster.ca_cert") {
    config.cluster.ca_cert = std::string(value);
  } else if (key == "cluster.allow_insecure") {
    auto v = ParseBool(key, value);
    if (!v.ok())
      return v.status();
    config.cluster.allow_insecure = *v;
  } else if (key == "server.listen") {
    config.server.listen = std::string(value);
  } else if (key == "server.advertise") {
    config.server.advertise = std::string(value);
  } else if (key == "server.max_message_bytes") {
    auto v = ParseU64(key, value);
    if (!v.ok())
      return v.status();
    config.server.max_message_bytes = *v;
  } else if (key == "server.tls_cert") {
    config.server.tls_cert = std::string(value);
  } else if (key == "server.tls_key") {
    config.server.tls_key = std::string(value);
  } else if (key == "server.rate_limit_per_second") {
    auto v = ParseU32(key, value);
    if (!v.ok())
      return v.status();
    config.server.rate_limit_per_second = *v;
  } else if (key == "server.rate_limit_burst") {
    auto v = ParseU32(key, value);
    if (!v.ok())
      return v.status();
    config.server.rate_limit_burst = *v;
  } else if (key == "client.ca_cert") {
    config.client.ca_cert = std::string(value);
  } else if (key == "client.tls_cert") {
    config.client.tls_cert = std::string(value);
  } else if (key == "client.tls_key") {
    config.client.tls_key = std::string(value);
  } else if (key == "client.server_name") {
    config.client.server_name = std::string(value);
  } else if (key == "client.allow_insecure") {
    auto v = ParseBool(key, value);
    if (!v.ok())
      return v.status();
    config.client.allow_insecure = *v;
  } else if (key == "crypto.poly_modulus_degree") {
    auto v = ParseU32(key, value);
    if (!v.ok())
      return v.status();
    config.crypto.poly_modulus_degree = *v;
  } else if (key == "crypto.plain_modulus_bits") {
    auto v = ParseU32(key, value);
    if (!v.ok())
      return v.status();
    config.crypto.plain_modulus_bits = *v;
  } else if (key == "crypto.coeff_modulus_bits") {
    auto v = ParseIntList(key, value);
    if (!v.ok())
      return v.status();
    config.crypto.coeff_modulus_bits = *v;
  } else if (key == "crypto.key_bits") {
    auto v = ParseU32(key, value);
    if (!v.ok())
      return v.status();
    config.crypto.key_bits = *v;
  } else if (key == "crypto.result_buckets") {
    auto v = ParseU32(key, value);
    if (!v.ok())
      return v.status();
    config.crypto.result_buckets = *v;
  } else if (key == "storage.record_bytes") {
    auto v = ParseU32(key, value);
    if (!v.ok())
      return v.status();
    config.storage.record_bytes = *v;
  } else if (key == "storage.epoch_dir") {
    config.storage.epoch_dir = std::string(value);
  } else if (key == "auth.mode") {
    auto v = ParseAuthMode(value);
    if (!v.ok())
      return v.status();
    config.auth.mode = *v;
  } else if (key == "auth.enable_insecure") {
    auto v = ParseBool(key, value);
    if (!v.ok())
      return v.status();
    config.auth.enable_insecure = *v;
  } else if (key == "auth.token_file") {
    config.auth.token_file = std::string(value);
  } else if (key == "auth.ca_cert") {
    config.auth.ca_cert = std::string(value);
  } else if (key == "auth.admin_identities") {
    config.auth.admin_identities = ParseStringList(value);
  } else if (key == "blobstore.path") {
    config.blobstore.path = std::string(value);
  } else if (key == "metrics.listen") {
    config.metrics.listen = std::string(value);
  } else if (key == "logging.level") {
    config.logging.level = std::string(value);
  } else if (key == "logging.format") {
    auto v = ParseLogFormat(value);
    if (!v.ok())
      return v.status();
    config.logging.format = *v;
  } else if (key == "logging.file") {
    config.logging.file = std::string(value);
  } else {
    return absl::InvalidArgumentError(
        absl::StrCat("unknown config key '", key, "'"));
  }
  return absl::OkStatus();
}

absl::Status Validate(const Config &config) {
  const CryptoConfig &c = config.crypto;
  if (!IsPowerOfTwo(c.poly_modulus_degree)) {
    return absl::InvalidArgumentError(
        absl::StrCat("crypto.poly_modulus_degree must be a power of two, got ",
                     c.poly_modulus_degree));
  }
  if (c.poly_modulus_degree < 1024 || c.poly_modulus_degree > 32768) {
    return absl::InvalidArgumentError(absl::StrCat(
        "crypto.poly_modulus_degree must be in [1024, 32768], got ",
        c.poly_modulus_degree));
  }
  if (c.plain_modulus_bits < 2 || c.plain_modulus_bits > 60) {
    return absl::InvalidArgumentError(
        absl::StrCat("crypto.plain_modulus_bits must be in [2, 60], got ",
                     c.plain_modulus_bits));
  }
  if (c.coeff_modulus_bits.empty()) {
    return absl::InvalidArgumentError(
        "crypto.coeff_modulus_bits must list at least one prime size");
  }
  for (int bits : c.coeff_modulus_bits) {
    if (bits < 1 || bits > 60) {
      return absl::InvalidArgumentError(absl::StrCat(
          "crypto.coeff_modulus_bits entries must be in [1, 60], got ", bits));
    }
  }
  if (c.key_bits == 0 || !IsPowerOfTwo(c.key_bits)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "crypto.key_bits must be a positive power of two, got ", c.key_bits));
  }
  if (c.key_bits > 64) {
    return absl::InvalidArgumentError(
        absl::StrCat("crypto.key_bits must be at most 64, got ", c.key_bits));
  }
  // Each record occupies key_bits slots, so a record block must fit in one
  // BatchEncoder row (poly_modulus_degree / 2 slots).
  if (c.key_bits > c.poly_modulus_degree / 2) {
    return absl::InvalidArgumentError(
        absl::StrCat("crypto.key_bits (", c.key_bits,
                     ") must not exceed poly_modulus_degree/2 (",
                     c.poly_modulus_degree / 2, ")"));
  }
  if (c.result_buckets == 0 || !IsPowerOfTwo(c.result_buckets)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "crypto.result_buckets must be a positive power of two, got ",
        c.result_buckets));
  }
  // Each bucket is one block (key_bits slots) wide after the partial block-sum,
  // so the bucket count cannot exceed the number of blocks in one BatchEncoder
  // row. With both a power of two this also guarantees it divides evenly.
  if (const std::uint32_t blocks_per_row =
          (c.poly_modulus_degree / 2) / c.key_bits;
      c.result_buckets > blocks_per_row) {
    return absl::InvalidArgumentError(
        absl::StrCat("crypto.result_buckets (", c.result_buckets,
                     ") must not exceed (poly_modulus_degree/2)/key_bits (",
                     blocks_per_row, ")"));
  }
  if (config.storage.record_bytes == 0) {
    return absl::InvalidArgumentError("storage.record_bytes must be positive");
  }
  if (config.server.max_message_bytes == 0) {
    return absl::InvalidArgumentError(
        "server.max_message_bytes must be positive");
  }
  if (config.server.rate_limit_per_second == 0 ||
      config.server.rate_limit_burst == 0) {
    return absl::InvalidArgumentError(
        "server.rate_limit_per_second and server.rate_limit_burst must be "
        "positive");
  }
  // A client certificate is a key pair: one without the other cannot complete a
  // handshake, so reject the half-configured case early.
  if (config.client.tls_cert.empty() != config.client.tls_key.empty()) {
    return absl::InvalidArgumentError(
        "client.tls_cert and client.tls_key must be set together");
  }
  // A client certificate is only usable when the client also has a CA to verify
  // the server; mutual TLS needs both directions.
  if (!config.client.tls_cert.empty() && config.client.ca_cert.empty()) {
    return absl::InvalidArgumentError(
        "client mTLS (client.tls_cert) requires client.ca_cert to verify the "
        "server");
  }
  // A clustered node exchanges client ciphertexts and keys with peers over the
  // Internal RPC. Refuse to do that in the clear: require cluster mTLS (its own
  // certs) or, failing that, server TLS, unless the operator explicitly opts
  // into insecure node-to-node traffic. This fails closed by default.
  if (config.cluster.enabled) {
    const ClusterConfig &cl = config.cluster;
    const bool cluster_tls =
        !cl.tls_cert.empty() && !cl.tls_key.empty() && !cl.ca_cert.empty();
    const bool server_tls =
        !config.server.tls_cert.empty() && !config.server.tls_key.empty();
    if (!cluster_tls && !server_tls && !cl.allow_insecure) {
      return absl::FailedPreconditionError(
          "cluster.enabled requires cluster mTLS (cluster.tls_cert, "
          "cluster.tls_key, cluster.ca_cert) or server TLS; set "
          "cluster.allow_insecure = true only for local development");
    }
    if (!cl.tls_cert.empty() && (cl.tls_key.empty() || cl.ca_cert.empty())) {
      return absl::InvalidArgumentError(
          "cluster.tls_cert requires cluster.tls_key and cluster.ca_cert");
    }
  }
  if (config.auth.mode == AuthMode::kNone && !config.auth.enable_insecure) {
    return absl::FailedPreconditionError(
        "auth.mode = none requires auth.enable_insecure = true; refusing to "
        "start an open node by accident");
  }
  if (config.auth.mode == AuthMode::kMtls) {
    if (config.server.tls_cert.empty() || config.server.tls_key.empty()) {
      return absl::InvalidArgumentError(
          "auth.mode = mtls requires server.tls_cert and server.tls_key");
    }
    if (config.auth.ca_cert.empty()) {
      return absl::InvalidArgumentError(
          "auth.mode = mtls requires auth.ca_cert to verify client certs");
    }
  }
  static constexpr std::string_view kLevels[] = {
      "trace", "debug", "info", "warn", "error", "critical", "off"};
  bool level_ok = false;
  for (std::string_view lvl : kLevels) {
    if (config.logging.level == lvl)
      level_ok = true;
  }
  if (!level_ok) {
    return absl::InvalidArgumentError(absl::StrCat(
        "logging.level '", config.logging.level,
        "' is not one of trace|debug|info|warn|error|critical|off"));
  }
  return absl::OkStatus();
}

std::string ResolveConfigPath(std::string_view config_flag,
                              const EnvLookup &env) {
  if (!config_flag.empty())
    return std::string(config_flag);
  if (std::optional<std::string> from_env = env("OPAQUEDB_CONFIG")) {
    return *from_env;
  }
  return std::string(kDefaultConfigPath);
}

absl::StatusOr<Config> Load(const LoadOptions &options) {
  Config config; // built-in defaults
  if (!options.config_path.empty()) {
    if (absl::Status s = ApplyTomlFile(config, options.config_path,
                                       options.file_is_optional);
        !s.ok()) {
      return s;
    }
  }
  if (absl::Status s = ApplyEnv(config, options.env); !s.ok()) {
    return s;
  }
  for (const auto &[key, value] : options.flag_overrides) {
    if (absl::Status s = ApplyKeyValue(config, key, value); !s.ok()) {
      return s;
    }
  }
  if (absl::Status s = Validate(config); !s.ok()) {
    return s;
  }
  return config;
}

absl::StatusOr<Config> LoadFileOnly(std::string_view config_path) {
  Config config;
  if (absl::Status s = ApplyTomlFile(config, config_path, /*optional=*/false);
      !s.ok()) {
    return s;
  }
  if (absl::Status s = Validate(config); !s.ok()) {
    return s;
  }
  return config;
}

std::string ToToml(const Config &config) {
  toml::array endpoints;
  for (const std::string &e : config.cluster.etcd_endpoints) {
    endpoints.push_back(e);
  }
  toml::array coeff;
  for (int bits : config.crypto.coeff_modulus_bits) {
    coeff.push_back(static_cast<std::int64_t>(bits));
  }
  toml::array admin_identities;
  for (const std::string &id : config.auth.admin_identities) {
    admin_identities.push_back(id);
  }

  toml::table root;
  root.insert("node", toml::table{{"id", config.node.id},
                                  {"data_dir", config.node.data_dir}});
  root.insert("cluster",
              toml::table{{"enabled", config.cluster.enabled},
                          {"etcd_endpoints", std::move(endpoints)},
                          {"leader_key", config.cluster.leader_key},
                          {"etcd_username", config.cluster.etcd_username},
                          {"etcd_password", config.cluster.etcd_password},
                          {"etcd_ca_cert", config.cluster.etcd_ca_cert},
                          {"etcd_client_cert", config.cluster.etcd_client_cert},
                          {"etcd_client_key", config.cluster.etcd_client_key},
                          {"etcd_tls_name", config.cluster.etcd_tls_name},
                          {"listen", config.cluster.listen},
                          {"advertise", config.cluster.advertise},
                          {"tls_cert", config.cluster.tls_cert},
                          {"tls_key", config.cluster.tls_key},
                          {"ca_cert", config.cluster.ca_cert},
                          {"allow_insecure", config.cluster.allow_insecure}});
  root.insert(
      "server",
      toml::table{
          {"listen", config.server.listen},
          {"advertise", config.server.advertise},
          {"max_message_bytes",
           static_cast<std::int64_t>(config.server.max_message_bytes)},
          {"tls_cert", config.server.tls_cert},
          {"tls_key", config.server.tls_key},
          {"rate_limit_per_second",
           static_cast<std::int64_t>(config.server.rate_limit_per_second)},
          {"rate_limit_burst",
           static_cast<std::int64_t>(config.server.rate_limit_burst)}});
  root.insert("client",
              toml::table{{"ca_cert", config.client.ca_cert},
                          {"tls_cert", config.client.tls_cert},
                          {"tls_key", config.client.tls_key},
                          {"server_name", config.client.server_name},
                          {"allow_insecure", config.client.allow_insecure}});
  root.insert(
      "crypto",
      toml::table{
          {"poly_modulus_degree",
           static_cast<std::int64_t>(config.crypto.poly_modulus_degree)},
          {"plain_modulus_bits",
           static_cast<std::int64_t>(config.crypto.plain_modulus_bits)},
          {"coeff_modulus_bits", std::move(coeff)},
          {"key_bits", static_cast<std::int64_t>(config.crypto.key_bits)},
          {"result_buckets",
           static_cast<std::int64_t>(config.crypto.result_buckets)}});
  root.insert("storage",
              toml::table{{"record_bytes", static_cast<std::int64_t>(
                                               config.storage.record_bytes)},
                          {"epoch_dir", config.storage.epoch_dir}});
  root.insert("auth",
              toml::table{{"mode", ToString(config.auth.mode)},
                          {"enable_insecure", config.auth.enable_insecure},
                          {"token_file", config.auth.token_file},
                          {"ca_cert", config.auth.ca_cert},
                          {"admin_identities", std::move(admin_identities)}});
  root.insert("blobstore", toml::table{{"path", config.blobstore.path}});
  root.insert("metrics", toml::table{{"listen", config.metrics.listen}});
  root.insert("logging",
              toml::table{{"level", config.logging.level},
                          {"format", ToString(config.logging.format)},
                          {"file", config.logging.file}});

  std::ostringstream out;
  out << root << '\n';
  return out.str();
}

} // namespace opaquedb::config

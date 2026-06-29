#include "opaquedb/config/loader.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "opaquedb/config/config.h"

namespace {

using opaquedb::config::AuthMode;
using opaquedb::config::Config;
using opaquedb::config::EnvLookup;
using opaquedb::config::Load;
using opaquedb::config::LoadOptions;

// An injected environment over a fixed map, so tests need no real getenv.
EnvLookup FakeEnv(std::map<std::string, std::string> vars) {
  return [vars = std::move(vars)](
             std::string_view name) -> std::optional<std::string> {
    auto it = vars.find(std::string(name));
    if (it == vars.end())
      return std::nullopt;
    return it->second;
  };
}

// Writes text to a unique temp file and returns its path. Registered for
// cleanup by the fixture.
class ConfigTest : public ::testing::Test {
protected:
  std::string WriteTemp(std::string_view contents) {
    std::filesystem::path p =
        std::filesystem::temp_directory_path() /
        ("opaquedb_cfg_" + std::to_string(counter_++) + ".toml");
    std::ofstream(p) << contents;
    paths_.push_back(p);
    return p.string();
  }

  void TearDown() override {
    for (const auto &p : paths_) {
      std::error_code ec;
      std::filesystem::remove(p, ec);
    }
  }

private:
  int counter_ = 0;
  std::vector<std::filesystem::path> paths_;
};

TEST_F(ConfigTest, DefaultsLoadAndValidate) {
  LoadOptions opts;
  opts.env = FakeEnv({});
  auto cfg = Load(opts);
  ASSERT_TRUE(cfg.ok()) << cfg.status().message();
  EXPECT_EQ(cfg->server.listen, "0.0.0.0:50051");
  EXPECT_EQ(cfg->crypto.poly_modulus_degree, 16384u);
  EXPECT_EQ(cfg->crypto.key_bits, 16u);
  EXPECT_EQ(cfg->auth.mode, AuthMode::kToken);
}

TEST_F(ConfigTest, FileOverridesDefaults) {
  std::string path = WriteTemp(R"([server]
listen = "0.0.0.0:6000"
[crypto]
coeff_modulus_bits = [60, 40, 40, 40, 60]
)");
  LoadOptions opts;
  opts.config_path = path;
  opts.env = FakeEnv({});
  auto cfg = Load(opts);
  ASSERT_TRUE(cfg.ok()) << cfg.status().message();
  EXPECT_EQ(cfg->server.listen, "0.0.0.0:6000");
  EXPECT_EQ(cfg->crypto.coeff_modulus_bits,
            (std::vector<int>{60, 40, 40, 40, 60}));
}

TEST_F(ConfigTest, EnvOverridesFile) {
  std::string path = WriteTemp("[server]\nlisten = \"0.0.0.0:6000\"\n");
  LoadOptions opts;
  opts.config_path = path;
  opts.env = FakeEnv({{"OPAQUEDB_SERVER_LISTEN", "0.0.0.0:7000"}});
  auto cfg = Load(opts);
  ASSERT_TRUE(cfg.ok()) << cfg.status().message();
  EXPECT_EQ(cfg->server.listen, "0.0.0.0:7000");
}

TEST_F(ConfigTest, FlagOverridesEnvAndFile) {
  std::string path = WriteTemp("[server]\nlisten = \"0.0.0.0:6000\"\n");
  LoadOptions opts;
  opts.config_path = path;
  opts.env = FakeEnv({{"OPAQUEDB_SERVER_LISTEN", "0.0.0.0:7000"}});
  opts.flag_overrides = {{"server.listen", "0.0.0.0:8000"}};
  auto cfg = Load(opts);
  ASSERT_TRUE(cfg.ok()) << cfg.status().message();
  EXPECT_EQ(cfg->server.listen, "0.0.0.0:8000");
}

TEST_F(ConfigTest, FullPrecedenceChainPerKey) {
  // node.id only in file, data_dir only in env, server.listen via flag.
  std::string path = WriteTemp("[node]\nid = \"from-file\"\n");
  LoadOptions opts;
  opts.config_path = path;
  opts.env = FakeEnv({{"OPAQUEDB_NODE_DATA_DIR", "/env/dir"}});
  opts.flag_overrides = {{"server.listen", "0.0.0.0:9999"}};
  auto cfg = Load(opts);
  ASSERT_TRUE(cfg.ok()) << cfg.status().message();
  EXPECT_EQ(cfg->node.id, "from-file");
  EXPECT_EQ(cfg->node.data_dir, "/env/dir");
  EXPECT_EQ(cfg->server.listen, "0.0.0.0:9999");
}

TEST_F(ConfigTest, EnvParsesListsAsCsv) {
  LoadOptions opts;
  opts.env = FakeEnv(
      {{"OPAQUEDB_CLUSTER_ETCD_ENDPOINTS", "http://a:2379,http://b:2379"},
       {"OPAQUEDB_CRYPTO_COEFF_MODULUS_BITS", "60, 40, 40, 40, 60"}});
  auto cfg = Load(opts);
  ASSERT_TRUE(cfg.ok()) << cfg.status().message();
  EXPECT_EQ(cfg->cluster.etcd_endpoints,
            (std::vector<std::string>{"http://a:2379", "http://b:2379"}));
  EXPECT_EQ(cfg->crypto.coeff_modulus_bits,
            (std::vector<int>{60, 40, 40, 40, 60}));
}

TEST_F(ConfigTest, RejectsNonPowerOfTwoPolyDegree) {
  LoadOptions opts;
  opts.env = FakeEnv({{"OPAQUEDB_CRYPTO_POLY_MODULUS_DEGREE", "5000"}});
  auto cfg = Load(opts);
  ASSERT_FALSE(cfg.ok());
  EXPECT_EQ(cfg.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(ConfigTest, RejectsZeroRecordBytes) {
  LoadOptions opts;
  opts.env = FakeEnv({{"OPAQUEDB_STORAGE_RECORD_BYTES", "0"}});
  auto cfg = Load(opts);
  ASSERT_FALSE(cfg.ok());
}

TEST_F(ConfigTest, RejectsNonPowerOfTwoKeyBits) {
  LoadOptions opts;
  opts.env =
      FakeEnv({{"OPAQUEDB_CRYPTO_KEY_BITS", "20"}}); // not a power of two
  auto cfg = Load(opts);
  ASSERT_FALSE(cfg.ok());
  EXPECT_EQ(cfg.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(ConfigTest, AcceptsPowerOfTwoKeyBits) {
  LoadOptions opts;
  opts.env = FakeEnv({{"OPAQUEDB_CRYPTO_KEY_BITS", "32"}});
  auto cfg = Load(opts);
  ASSERT_TRUE(cfg.ok()) << cfg.status().message();
  EXPECT_EQ(cfg->crypto.key_bits, 32u);
}

TEST_F(ConfigTest, RejectsKeyBitsTooDeepForModulusChain) {
  // key_bits = 64 is a power of two <= 64 and fits the slot geometry, but its
  // matcher depth (1 + log2(64) = 7) exceeds the default 6-prime modulus chain,
  // which used to be accepted and silently decrypt every query to garbage.
  LoadOptions opts;
  opts.env = FakeEnv({{"OPAQUEDB_CRYPTO_KEY_BITS", "64"}});
  auto cfg = Load(opts);
  ASSERT_FALSE(cfg.ok());
  EXPECT_EQ(cfg.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(ConfigTest, AcceptsKeyBits64WhenChainIsDeepEnough) {
  // The same key_bits = 64 is fine once the chain lists enough primes for the
  // depth-7 matcher (the guard is about chain depth, not a hard key_bits cap).
  LoadOptions opts;
  opts.env =
      FakeEnv({{"OPAQUEDB_CRYPTO_KEY_BITS", "64"},
               {"OPAQUEDB_CRYPTO_COEFF_MODULUS_BITS", "60,60,60,60,60,60,49"}});
  auto cfg = Load(opts);
  ASSERT_TRUE(cfg.ok()) << cfg.status().message();
  EXPECT_EQ(cfg->crypto.key_bits, 64u);
}

TEST_F(ConfigTest, ClusterEnabledRequiresMtlsOrOptIn) {
  // A clustered node with no cluster/server TLS fails closed.
  LoadOptions deny;
  deny.env = FakeEnv({{"OPAQUEDB_CLUSTER_ENABLED", "true"}});
  auto cfg = Load(deny);
  ASSERT_FALSE(cfg.ok());
  EXPECT_EQ(cfg.status().code(), absl::StatusCode::kFailedPrecondition);

  // The explicit insecure opt-in allows it (local development).
  LoadOptions allow;
  allow.env = FakeEnv({{"OPAQUEDB_CLUSTER_ENABLED", "true"},
                       {"OPAQUEDB_CLUSTER_ALLOW_INSECURE", "true"}});
  EXPECT_TRUE(Load(allow).ok());
}

TEST_F(ConfigTest, AuthNoneNeedsInsecureFlag) {
  LoadOptions deny;
  deny.env = FakeEnv({{"OPAQUEDB_AUTH_MODE", "none"}});
  EXPECT_FALSE(Load(deny).ok());

  LoadOptions allow;
  allow.env = FakeEnv({{"OPAQUEDB_AUTH_MODE", "none"},
                       {"OPAQUEDB_AUTH_ENABLE_INSECURE", "true"}});
  EXPECT_TRUE(Load(allow).ok());
}

TEST_F(ConfigTest, MtlsNeedsCertsAndCa) {
  LoadOptions opts;
  opts.env = FakeEnv({{"OPAQUEDB_AUTH_MODE", "mtls"}});
  EXPECT_FALSE(Load(opts).ok());

  opts.env = FakeEnv({{"OPAQUEDB_AUTH_MODE", "mtls"},
                      {"OPAQUEDB_SERVER_TLS_CERT", "/c"},
                      {"OPAQUEDB_SERVER_TLS_KEY", "/k"},
                      {"OPAQUEDB_AUTH_CA_CERT", "/ca"}});
  EXPECT_TRUE(Load(opts).ok());
}

TEST_F(ConfigTest, ClientCertPairMustBeSetTogether) {
  LoadOptions half;
  half.env = FakeEnv(
      {{"OPAQUEDB_CLIENT_TLS_CERT", "/c"}, {"OPAQUEDB_CLIENT_CA_CERT", "/ca"}});
  EXPECT_FALSE(Load(half).ok()); // cert without key

  LoadOptions full;
  full.env = FakeEnv({{"OPAQUEDB_CLIENT_TLS_CERT", "/c"},
                      {"OPAQUEDB_CLIENT_TLS_KEY", "/k"},
                      {"OPAQUEDB_CLIENT_CA_CERT", "/ca"}});
  EXPECT_TRUE(Load(full).ok());
}

TEST_F(ConfigTest, ClientMtlsRequiresCaCert) {
  LoadOptions opts;
  opts.env = FakeEnv(
      {{"OPAQUEDB_CLIENT_TLS_CERT", "/c"}, {"OPAQUEDB_CLIENT_TLS_KEY", "/k"}});
  EXPECT_FALSE(Load(opts).ok()); // client cert but no CA to verify the server
}

TEST_F(ConfigTest, RejectsZeroRateLimit) {
  LoadOptions opts;
  opts.env = FakeEnv({{"OPAQUEDB_SERVER_RATE_LIMIT_PER_SECOND", "0"}});
  EXPECT_FALSE(Load(opts).ok());
}

TEST_F(ConfigTest, AdminIdentitiesRoundTrip) {
  LoadOptions opts;
  opts.env = FakeEnv({{"OPAQUEDB_AUTH_MODE", "mtls"},
                      {"OPAQUEDB_SERVER_TLS_CERT", "/c"},
                      {"OPAQUEDB_SERVER_TLS_KEY", "/k"},
                      {"OPAQUEDB_AUTH_CA_CERT", "/ca"},
                      {"OPAQUEDB_AUTH_ADMIN_IDENTITIES", "cn=root,cn=ops"}});
  auto cfg = Load(opts);
  ASSERT_TRUE(cfg.ok()) << cfg.status().message();
  EXPECT_EQ(cfg->auth.admin_identities,
            (std::vector<std::string>{"cn=root", "cn=ops"}));
  // Survives a ToToml -> LoadFileOnly round trip.
  std::string path = WriteTemp(opaquedb::config::ToToml(*cfg));
  auto reloaded = opaquedb::config::LoadFileOnly(path);
  ASSERT_TRUE(reloaded.ok()) << reloaded.status().message();
  EXPECT_EQ(reloaded->auth.admin_identities,
            (std::vector<std::string>{"cn=root", "cn=ops"}));
}

TEST_F(ConfigTest, RejectsUnknownKey) {
  Config c;
  EXPECT_FALSE(opaquedb::config::ApplyKeyValue(c, "server.bogus", "x").ok());
}

TEST_F(ConfigTest, MissingFileIsErrorUnlessOptional) {
  LoadOptions opts;
  opts.config_path = "/no/such/opaquedb.toml";
  opts.env = FakeEnv({});
  EXPECT_FALSE(Load(opts).ok());

  opts.file_is_optional = true;
  EXPECT_TRUE(Load(opts).ok());
}

TEST_F(ConfigTest, ToTomlRoundTrips) {
  LoadOptions opts;
  opts.env =
      FakeEnv({{"OPAQUEDB_SERVER_LISTEN", "0.0.0.0:5555"},
               {"OPAQUEDB_CRYPTO_COEFF_MODULUS_BITS", "60,40,40,40,60"}});
  auto cfg = Load(opts);
  ASSERT_TRUE(cfg.ok()) << cfg.status().message();

  std::string path = WriteTemp(opaquedb::config::ToToml(*cfg));
  auto reloaded = opaquedb::config::LoadFileOnly(path);
  ASSERT_TRUE(reloaded.ok()) << reloaded.status().message();
  EXPECT_EQ(reloaded->server.listen, "0.0.0.0:5555");
  EXPECT_EQ(reloaded->crypto.coeff_modulus_bits,
            (std::vector<int>{60, 40, 40, 40, 60}));
}

TEST_F(ConfigTest, ResolveConfigPathPrecedence) {
  auto env = FakeEnv({{"OPAQUEDB_CONFIG", "/from/env.toml"}});
  EXPECT_EQ(opaquedb::config::ResolveConfigPath("/from/flag.toml", env),
            "/from/flag.toml");
  EXPECT_EQ(opaquedb::config::ResolveConfigPath("", env), "/from/env.toml");
  EXPECT_EQ(opaquedb::config::ResolveConfigPath("", FakeEnv({})),
            opaquedb::config::kDefaultConfigPath);
}

TEST_F(ConfigTest, DefaultConfigTomlMatchesCompiledDefaults) {
  // The embedded canonical file is the single source of truth for the defaults.
  // It must parse and must equal the Config struct's member-initializer
  // defaults, so the two never drift.
  std::string path = WriteTemp(opaquedb::config::DefaultConfigToml());
  auto from_file = opaquedb::config::LoadFileOnly(path);
  ASSERT_TRUE(from_file.ok()) << from_file.status().message();

  const opaquedb::config::Config &f = *from_file;
  opaquedb::config::Config d; // built-in defaults
  EXPECT_EQ(f.node.data_dir, d.node.data_dir);
  EXPECT_EQ(f.server.listen, d.server.listen);
  EXPECT_EQ(f.server.max_message_bytes, d.server.max_message_bytes);
  EXPECT_EQ(f.crypto.poly_modulus_degree, d.crypto.poly_modulus_degree);
  EXPECT_EQ(f.crypto.plain_modulus_bits, d.crypto.plain_modulus_bits);
  EXPECT_EQ(f.crypto.coeff_modulus_bits, d.crypto.coeff_modulus_bits);
  EXPECT_EQ(f.crypto.key_bits, d.crypto.key_bits);
  EXPECT_EQ(f.storage.record_bytes, d.storage.record_bytes);
  EXPECT_EQ(f.auth.token_file, d.auth.token_file);
  EXPECT_EQ(f.keyring.path, d.keyring.path);
  EXPECT_EQ(f.metrics.listen, d.metrics.listen);
  EXPECT_EQ(f.logging.level, d.logging.level);
}

} // namespace

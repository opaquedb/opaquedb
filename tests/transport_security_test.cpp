#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "opaquedb/client/query_client.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/server/ingest.h"
#include "opaquedb/server/node_server.h"
#include "opaquedb/storage/repository.h"

// Transport security end to end: a node with server TLS, a client that verifies
// it (and one that fails closed without TLS material), and a two-node cluster
// whose node-to-node fan-out runs on a separate mutual-TLS listener so only a
// cluster-CA-signed peer can reach Evaluate. Certificates are generated with
// the openssl CLI in the fixture; the suite skips if openssl is absent.

namespace {

namespace fs = std::filesystem;
using opaquedb::client::QueryClient;
using opaquedb::config::Config;
using opaquedb::core::Column;
using opaquedb::core::ColumnEncoding;
using opaquedb::core::ColumnType;
using opaquedb::core::Schema;
using opaquedb::core::Value;
using opaquedb::server::BuildStagingFromRows;
using opaquedb::server::IngestRow;
using opaquedb::server::NodeServer;
using opaquedb::storage::LocalEpochRepository;

Schema KvSchema() {
  return Schema("t", {Column{"k", ColumnEncoding::kEq, ColumnType::kInt},
                      Column{"v", ColumnEncoding::kRaw, ColumnType::kText}});
}

std::string TextValue(const std::vector<std::uint8_t> &record) {
  auto values = opaquedb::core::DecodeRecord(KvSchema(), record);
  if (!values.ok() || values->empty())
    return "";
  return opaquedb::core::ValueToString(values->front());
}

// Paths to one CA and a leaf cert/key signed by it. The leaf carries SANs for
// 127.0.0.1 and localhost so it verifies when dialing either.
struct Certs {
  std::string ca;
  std::string server_cert;
  std::string server_key;
  std::string client_cert;
  std::string client_key;
};

bool HaveOpenssl() {
  return std::system("openssl version >/dev/null 2>&1") == 0;
}

// Generates a CA plus a server and client leaf in dir using the openssl CLI.
// Returns false on any failure (the test then skips).
bool GenerateCerts(const fs::path &dir, Certs *out) {
  const std::string d = dir.string();
  std::ofstream(dir / "san.ext")
      << "subjectAltName=IP:127.0.0.1,DNS:localhost,DNS:opaquedb-node\n";
  const std::string script =
      "set -e; cd '" + d +
      "'; "
      "openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt "
      "-subj '/CN=opaquedb-test-ca' -days 2 >/dev/null 2>&1; "
      "for who in server client; do "
      "  openssl req -newkey rsa:2048 -nodes -keyout $who.key -out $who.csr "
      "    -subj \"/CN=opaquedb-$who\" >/dev/null 2>&1; "
      "  openssl x509 -req -in $who.csr -CA ca.crt -CAkey ca.key "
      "    -CAcreateserial -out $who.crt -days 2 -extfile san.ext "
      "    >/dev/null 2>&1; "
      "done";
  if (std::system(script.c_str()) != 0)
    return false;
  out->ca = (dir / "ca.crt").string();
  out->server_cert = (dir / "server.crt").string();
  out->server_key = (dir / "server.key").string();
  out->client_cert = (dir / "client.crt").string();
  out->client_key = (dir / "client.key").string();
  for (const std::string *p : {&out->ca, &out->server_cert, &out->server_key,
                               &out->client_cert, &out->client_key}) {
    if (!fs::exists(*p))
      return false;
  }
  return true;
}

class TransportSecurity : public ::testing::Test {
protected:
  void SetUp() override {
    if (!HaveOpenssl())
      GTEST_SKIP() << "openssl CLI not available";
    dir_ = fs::temp_directory_path() /
           ("opaquedb_tls_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(dir_);
    if (!GenerateCerts(dir_, &certs_))
      GTEST_SKIP() << "could not generate test certificates";
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  Config BaseNodeConfig(const std::string &subdir) {
    Config cfg;
    cfg.node.data_dir = (dir_ / subdir).string();
    cfg.storage.record_bytes = 32;
    cfg.server.listen = "127.0.0.1:0";
    return cfg;
  }

  void Publish(const Config &cfg,
               const std::vector<std::pair<std::uint64_t, std::string>> &kv) {
    std::vector<IngestRow> rows;
    for (const auto &[key, text] : kv)
      rows.push_back(
          IngestRow{Value{static_cast<std::int64_t>(key)}, {Value{text}}});
    auto staging = BuildStagingFromRows(cfg, KvSchema(), rows);
    ASSERT_TRUE(staging.ok()) << staging.status().message();
    auto repo = LocalEpochRepository::Open(cfg.EpochRootFor("default", "t"));
    ASSERT_TRUE(repo.ok());
    ASSERT_TRUE((*repo)->Publish(*staging, 1).ok());
  }

  fs::path dir_;
  Certs certs_;
};

// A node with server TLS and token auth; a client that verifies the server
// certificate runs a private query over the encrypted channel.
TEST_F(TransportSecurity, ServerTlsClientVerifies) {
  const std::string token_file = (dir_ / "tokens").string();
  { std::ofstream(token_file) << "alice query s3cret\n"; }

  Config cfg = BaseNodeConfig("node");
  cfg.auth.mode = opaquedb::config::AuthMode::kToken;
  cfg.auth.token_file = token_file;
  cfg.server.tls_cert = certs_.server_cert;
  cfg.server.tls_key = certs_.server_key;
  Publish(cfg, {{1, "one"}, {3, "three"}, {5, "five"}});

  auto node = NodeServer::Create(cfg);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  // The client verifies the server against the CA. The dial target is an IP, so
  // a SAN for 127.0.0.1 makes verification pass without a name override.
  Config client_cfg = cfg;
  client_cfg.client.ca_cert = certs_.ca;
  auto client =
      QueryClient::Create(client_cfg, (*node)->listen_address(), "s3cret");
  ASSERT_TRUE(client.ok()) << client.status().message();
  ASSERT_TRUE((*client)->Register("c1").ok());
  auto rows = (*client)->Query("c1", "SELECT v FROM t WHERE k = :k", 3, "");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ(TextValue((*rows)[0]), "three");

  (*node)->Shutdown();
}

// With no TLS material and no insecure opt-in, the client refuses to construct
// rather than sending its bearer token over a plaintext channel.
TEST_F(TransportSecurity, ClientFailsClosedWithoutTransport) {
  Config client_cfg;
  client_cfg.client.allow_insecure = false; // default, made explicit
  auto client = QueryClient::Create(client_cfg, "127.0.0.1:1", "tok");
  EXPECT_FALSE(client.ok());
  EXPECT_EQ(client.status().code(), absl::StatusCode::kFailedPrecondition);
}

// A two-node cluster with the Internal RPC on its own mutual-TLS listener: node
// A coordinates and fans out to node B over the cluster listener, which only
// admits a peer presenting a cluster-CA-signed certificate.
TEST_F(TransportSecurity, ClusterListenerMutualTlsFanOut) {
  auto cluster_cfg = [&](const std::string &subdir) {
    Config cfg = BaseNodeConfig(subdir);
    cfg.auth.mode = opaquedb::config::AuthMode::kNone;
    cfg.auth.enable_insecure = true; // client-facing side is open for the test
    cfg.client.allow_insecure = true;
    cfg.cluster.listen = "127.0.0.1:0"; // separate node-to-node listener
    cfg.cluster.tls_cert = certs_.server_cert;
    cfg.cluster.tls_key = certs_.server_key;
    cfg.cluster.ca_cert = certs_.ca;
    return cfg;
  };
  Config cfg_a = cluster_cfg("a");
  Config cfg_b = cluster_cfg("b");
  Publish(cfg_a, {{0, "value-0"}, {2, "value-2"}, {4, "value-4"}});
  Publish(cfg_b, {{1, "value-1"}, {3, "value-3"}, {5, "value-5"}});

  auto node_a = NodeServer::Create(cfg_a);
  auto node_b = NodeServer::Create(cfg_b);
  ASSERT_TRUE(node_a.ok() && node_b.ok());
  ASSERT_TRUE((*node_a)->Start().ok());
  ASSERT_TRUE((*node_b)->Start().ok());
  ASSERT_TRUE((*node_a)->WaitForReady().ok());
  ASSERT_TRUE((*node_b)->WaitForReady().ok());

  // The cluster listener address differs from the client listener address.
  EXPECT_NE((*node_b)->cluster_listen_address(), (*node_b)->listen_address());

  // A coordinates; its peer is B, reached on B's node-to-node listener.
  (*node_a)->SetQueryPeers({(*node_b)->cluster_listen_address()});

  auto client = QueryClient::Create(cfg_a, (*node_a)->listen_address());
  ASSERT_TRUE(client.ok()) << client.status().message();
  ASSERT_TRUE((*client)->Register("c1").ok());

  const std::string sql = "SELECT v FROM t WHERE k = :k";
  auto local = (*client)->Query("c1", sql, 4, ""); // node A's shard
  ASSERT_TRUE(local.ok()) << local.status().message();
  ASSERT_EQ(local->size(), 1u);
  EXPECT_EQ(TextValue((*local)[0]), "value-4");

  auto remote = (*client)->Query("c1", sql, 3, ""); // node B's shard, via mTLS
  ASSERT_TRUE(remote.ok()) << remote.status().message();
  ASSERT_EQ(remote->size(), 1u);
  EXPECT_EQ(TextValue((*remote)[0]), "value-3");

  (*node_a)->Shutdown();
  (*node_b)->Shutdown();
}

} // namespace

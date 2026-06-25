#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "admin.grpc.pb.h"
#include "grpcpp/grpcpp.h"
#include "opaquedb/admin/keyring_store.h"
#include "opaquedb/client/query_client.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/core/wire.h"
#include "opaquedb/server/engine.h"
#include "opaquedb/server/ingest.h"
#include "opaquedb/server/node_server.h"
#include "opaquedb/storage/repository.h"

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

// The fixture table: an integer key and one text value column.
Schema KvSchema() {
  return Schema("t", {Column{"k", ColumnEncoding::kEq, ColumnType::kInt},
                      Column{"v", ColumnEncoding::kRaw, ColumnType::kText}});
}

// Decodes the single text payload column from a returned record.
std::string TextValue(const std::vector<std::uint8_t> &record) {
  auto values = opaquedb::core::DecodeRecord(KvSchema(), record);
  if (!values.ok() || values->empty())
    return "";
  return opaquedb::core::ValueToString(values->front());
}

// One single-node end-to-end query proves the whole idea: a synthetic table is
// loaded, the node serves it, and the dev client gets the right row back with
// the WHERE value encrypted on the wire and decrypted only on the client.
class ServerEndToEnd : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("opaquedb_e2e_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(dir_);
    cfg_.node.data_dir = dir_.string();
    cfg_.storage.record_bytes = 32;     // small records keep the test quick
    cfg_.server.listen = "127.0.0.1:0"; // ephemeral port
    // This suite exercises the query path, not auth; run in open mode.
    cfg_.auth.mode = opaquedb::config::AuthMode::kNone;
    cfg_.auth.enable_insecure = true;
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  void LoadTable(std::uint64_t rows) {
    std::vector<IngestRow> data;
    for (std::uint64_t i = 0; i < rows; ++i) {
      data.push_back(
          IngestRow{Value{static_cast<std::int64_t>(i)},
                    {Value{std::string("value-" + std::to_string(i))}}});
    }
    auto staging = BuildStagingFromRows(cfg_, KvSchema(), data);
    ASSERT_TRUE(staging.ok()) << staging.status().message();
    auto repo = LocalEpochRepository::Open(cfg_.EpochRootFor("default", "t"));
    ASSERT_TRUE(repo.ok());
    ASSERT_TRUE((*repo)->Publish(*staging, 1).ok());
  }

  Config cfg_;
  fs::path dir_;
};

TEST_F(ServerEndToEnd, EngineCreateAlone) {
  LoadTable(4);
  opaquedb::admin::InMemoryKeyringStore keyring;
  auto repo = LocalEpochRepository::Open(cfg_.EpochRootFor("default", "t"));
  ASSERT_TRUE(repo.ok());
  auto engine =
      opaquedb::server::Engine::Create(cfg_.crypto, repo->get(), &keyring);
  ASSERT_TRUE(engine.ok()) << engine.status().message();
}

TEST_F(ServerEndToEnd, PrivateSelectReturnsTheRightRow) {
  LoadTable(8); // keys 0..7, values value-0..value-7

  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  auto client = QueryClient::Create(cfg_, (*node)->listen_address());
  ASSERT_TRUE(client.ok()) << client.status().message();
  ASSERT_TRUE((*client)->Register("c1").ok());

  // Query for key 3: the operator never sees the value 3 in plaintext.
  auto rows = (*client)->Query("c1", "SELECT v FROM t WHERE k = :k", 3, "");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ(TextValue((*rows)[0]), "value-3");

  (*node)->Shutdown();
}

TEST_F(ServerEndToEnd, AbsentKeyReturnsNoData) {
  LoadTable(8);
  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  auto client = QueryClient::Create(cfg_, (*node)->listen_address());
  ASSERT_TRUE(client.ok());
  ASSERT_TRUE((*client)->Register("c1").ok());

  // Key 1000 is in the key universe but not in the table: no row matches, so
  // the presence indicator is zero and the client returns an empty result.
  auto rows = (*client)->Query("c1", "SELECT v FROM t WHERE k = :k", 1000, "");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_EQ(rows->size(), 0u)
      << "absent key must yield no rows, not a zero row";

  (*node)->Shutdown();
}

// A TEXT match key: the user's headline query, SELECT ... WHERE city =
// "London", with the city encrypted on the wire as a hashed codeword.
TEST_F(ServerEndToEnd, PrivateSelectOnTextKey) {
  const Schema weather(
      "weather",
      {Column{"city", ColumnEncoding::kEq, ColumnType::kText},
       Column{"temperature", ColumnEncoding::kRaw, ColumnType::kInt}});
  std::vector<IngestRow> data = {
      IngestRow{Value{std::string("London")}, {Value{std::int64_t{11}}}},
      IngestRow{Value{std::string("Paris")}, {Value{std::int64_t{17}}}},
      IngestRow{Value{std::string("Tokyo")}, {Value{std::int64_t{27}}}}};
  auto staging = BuildStagingFromRows(cfg_, weather, data);
  ASSERT_TRUE(staging.ok()) << staging.status().message();
  auto repo =
      LocalEpochRepository::Open(cfg_.EpochRootFor("default", "weather"));
  ASSERT_TRUE(repo.ok());
  ASSERT_TRUE((*repo)->Publish(*staging, 1).ok());

  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  auto client = QueryClient::Create(cfg_, (*node)->listen_address());
  ASSERT_TRUE(client.ok()) << client.status().message();
  ASSERT_TRUE((*client)->Register("c1").ok());

  // The literal value never crosses the wire in plaintext; the client strips
  // and encrypts it. The bound-value argument is ignored when a literal is
  // present, so pass 0.
  auto rows = (*client)->Query(
      "c1", "SELECT temperature FROM weather WHERE city = \"London\"", 0, "");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  ASSERT_EQ(rows->size(), 1u);
  auto values = opaquedb::core::DecodeRecord(weather, (*rows)[0]);
  ASSERT_TRUE(values.ok()) << values.status().message();
  ASSERT_EQ(values->size(), 1u);
  EXPECT_EQ(std::get<std::int64_t>(values->front()), 11);

  (*node)->Shutdown();
}

// Two tables in two databases served by one node: each query routes to the
// right per-table repository. Proves multi-table and multi-database routing.
TEST_F(ServerEndToEnd, RoutesAcrossDatabasesAndTables) {
  const Schema weather(
      "weather",
      {Column{"city", ColumnEncoding::kEq, ColumnType::kText},
       Column{"temperature", ColumnEncoding::kRaw, ColumnType::kInt}});
  const Schema rates(
      "rates", {Column{"currency", ColumnEncoding::kEq, ColumnType::kText},
                Column{"rate", ColumnEncoding::kRaw, ColumnType::kInt}});

  auto publish = [&](const std::string &database, const Schema &schema,
                     const std::vector<IngestRow> &data) {
    auto staging = BuildStagingFromRows(cfg_, schema, data);
    ASSERT_TRUE(staging.ok()) << staging.status().message();
    auto repo =
        LocalEpochRepository::Open(cfg_.EpochRootFor(database, schema.table()));
    ASSERT_TRUE(repo.ok());
    ASSERT_TRUE((*repo)->Publish(*staging, 1).ok());
  };

  publish("default", weather,
          {IngestRow{Value{std::string("London")}, {Value{std::int64_t{11}}}},
           IngestRow{Value{std::string("Cairo")}, {Value{std::int64_t{33}}}}});
  publish("finance", rates,
          {IngestRow{Value{std::string("USD")}, {Value{std::int64_t{100}}}},
           IngestRow{Value{std::string("EUR")}, {Value{std::int64_t{108}}}}});

  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());
  auto client = QueryClient::Create(cfg_, (*node)->listen_address());
  ASSERT_TRUE(client.ok()) << client.status().message();
  ASSERT_TRUE((*client)->Register("c1").ok());

  // Default-database table, by inline literal.
  auto w = (*client)->Query(
      "c1", "SELECT temperature FROM weather WHERE city = \"Cairo\"", 0, "");
  ASSERT_TRUE(w.ok()) << w.status().message();
  ASSERT_EQ(w->size(), 1u);
  auto wv = opaquedb::core::DecodeRecord(weather, (*w)[0]);
  ASSERT_TRUE(wv.ok());
  EXPECT_EQ(std::get<std::int64_t>(wv->front()), 33);

  // Non-default database table, selected with the database argument.
  auto r =
      (*client)->Query("c1", "SELECT rate FROM rates WHERE currency = \"EUR\"",
                       0, "", "finance");
  ASSERT_TRUE(r.ok()) << r.status().message();
  ASSERT_EQ(r->size(), 1u);
  auto rv = opaquedb::core::DecodeRecord(rates, (*r)[0]);
  ASSERT_TRUE(rv.ok());
  EXPECT_EQ(std::get<std::int64_t>(rv->front()), 108);

  (*node)->Shutdown();
}

TEST_F(ServerEndToEnd, TokenAuthIsEnforced) {
  // Run the node in token mode with one known token.
  const std::string token_file = (dir_ / "tokens").string();
  { std::ofstream(token_file) << "alice query s3cret\n"; }
  cfg_.auth.mode = opaquedb::config::AuthMode::kToken;
  cfg_.auth.enable_insecure = false;
  cfg_.auth.token_file = token_file;

  LoadTable(8);
  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  // With the right token, the private query works.
  auto good = QueryClient::Create(cfg_, (*node)->listen_address(), "s3cret");
  ASSERT_TRUE(good.ok());
  ASSERT_TRUE((*good)->Register("c1").ok());
  auto rows = (*good)->Query("c1", "SELECT v FROM t WHERE k = :k", 4, "");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ(TextValue((*rows)[0]), "value-4");

  // Without a token, even registration is refused.
  auto anon = QueryClient::Create(cfg_, (*node)->listen_address(), "");
  ASSERT_TRUE(anon.ok());
  EXPECT_FALSE((*anon)->Register("c2").ok());

  // With a wrong token, refused.
  auto wrong = QueryClient::Create(cfg_, (*node)->listen_address(), "nope");
  ASSERT_TRUE(wrong.ok());
  EXPECT_FALSE((*wrong)->Register("c3").ok());

  (*node)->Shutdown();
}

TEST_F(ServerEndToEnd, AdminApiEnforcesAdminRoleOverGrpc) {
  const std::string token_file = (dir_ / "tokens").string();
  {
    std::ofstream out(token_file);
    out << "alice query q-token\n";
    out << "root admin a-token\n";
  }
  cfg_.auth.mode = opaquedb::config::AuthMode::kToken;
  cfg_.auth.enable_insecure = false;
  cfg_.auth.token_file = token_file;

  LoadTable(8);
  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  auto channel = grpc::CreateChannel((*node)->listen_address(),
                                     grpc::InsecureChannelCredentials());
  auto stub = opaquedb::proto::Admin::NewStub(channel);

  auto status_with = [&](const std::string &token) {
    grpc::ClientContext ctx;
    if (!token.empty())
      ctx.AddMetadata("authorization", "Bearer " + token);
    opaquedb::proto::StatusRequest req;
    req.set_wire_version(opaquedb::core::kWireVersion);
    opaquedb::proto::StatusReply reply;
    return stub->Status(&ctx, req, &reply).error_code();
  };

  // Admin token works; query token is forbidden; no token is unauthenticated.
  EXPECT_EQ(status_with("a-token"), grpc::StatusCode::OK);
  EXPECT_EQ(status_with("q-token"), grpc::StatusCode::PERMISSION_DENIED);
  EXPECT_EQ(status_with(""), grpc::StatusCode::UNAUTHENTICATED);

  (*node)->Shutdown();
}

TEST_F(ServerEndToEnd, UnregisteredClientIsRejected) {
  LoadTable(4);
  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  auto client = QueryClient::Create(cfg_, (*node)->listen_address());
  ASSERT_TRUE(client.ok());
  // No Register call: the engine has no keys for this client.
  auto rows = (*client)->Query("ghost", "SELECT v FROM t WHERE k = :k", 1, "");
  EXPECT_FALSE(rows.ok());

  (*node)->Shutdown();
}

} // namespace

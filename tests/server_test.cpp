#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "admin.grpc.pb.h"
#include "grpcpp/grpcpp.h"
#include "opaquedb.grpc.pb.h"
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
    // This suite exercises the query path, not auth; run in open mode with a
    // plaintext client channel (the transport-security suite covers TLS).
    cfg_.auth.mode = opaquedb::config::AuthMode::kNone;
    cfg_.auth.enable_insecure = true;
    cfg_.client.allow_insecure = true;
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

// WHERE k IN (...) returns every listed row, and SELECT COUNT(*) returns the
// exact match count from the encrypted presence ciphertext. End to end: the
// client lifts the inline literals, encrypts one operand per value, the matcher
// unions them, and the client decodes rows or sums the count.
TEST_F(ServerEndToEnd, InListAndCountStar) {
  LoadTable(8); // keys 0..7, values value-0..value-7

  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  auto client = QueryClient::Create(cfg_, (*node)->listen_address());
  ASSERT_TRUE(client.ok()) << client.status().message();
  ASSERT_TRUE((*client)->Register("c1").ok());

  // IN returns both listed rows (LIMIT high enough to take them all).
  auto rows = (*client)->Query(
      "c1", "SELECT v FROM t WHERE k IN (2, 5) LIMIT 10", 0, "");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  std::vector<std::string> got;
  for (const std::vector<std::uint8_t> &r : *rows)
    got.push_back(TextValue(r));
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, (std::vector<std::string>{"value-2", "value-5"}));

  // COUNT(*) over the IN list is exactly 2; absent key 0; unique key 1.
  auto two = (*client)->QueryCount(
      "c1", "SELECT COUNT(*) FROM t WHERE k IN (2, 5)", 0);
  ASSERT_TRUE(two.ok()) << two.status().message();
  EXPECT_EQ(*two, 2u);
  auto none =
      (*client)->QueryCount("c1", "SELECT COUNT(*) FROM t WHERE k = :k", 1000);
  ASSERT_TRUE(none.ok()) << none.status().message();
  EXPECT_EQ(*none, 0u);
  auto one =
      (*client)->QueryCount("c1", "SELECT COUNT(*) FROM t WHERE k = :k", 3);
  ASSERT_TRUE(one.ok()) << one.status().message();
  EXPECT_EQ(*one, 1u);

  (*node)->Shutdown();
}

// Several rows share one key. A LIMIT query partitions matches into buckets and
// returns all of them; this is the end-to-end multi-result path from SQL
// through the matcher to the client decode.
TEST_F(ServerEndToEnd, LimitReturnsAllRowsSharingAKey) {
  std::vector<IngestRow> data;
  for (int i = 0; i < 5; ++i) {
    data.push_back(IngestRow{Value{std::int64_t{7}},
                             {Value{std::string("dup-" + std::to_string(i))}}});
  }
  data.push_back(
      IngestRow{Value{std::int64_t{1}}, {Value{std::string("one")}}});
  auto staging = BuildStagingFromRows(cfg_, KvSchema(), data);
  ASSERT_TRUE(staging.ok()) << staging.status().message();
  auto repo = LocalEpochRepository::Open(cfg_.EpochRootFor("default", "t"));
  ASSERT_TRUE(repo.ok());
  ASSERT_TRUE((*repo)->Publish(*staging, 1).ok());

  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  auto client = QueryClient::Create(cfg_, (*node)->listen_address());
  ASSERT_TRUE(client.ok()) << client.status().message();
  ASSERT_TRUE((*client)->Register("c1").ok());

  auto query = [&](const std::string &sql) {
    std::uint32_t collided = 0;
    auto rows = (*client)->Query("c1", sql, 7, "", "default", &collided);
    EXPECT_TRUE(rows.ok()) << rows.status().message();
    std::vector<std::string> got;
    for (const std::vector<std::uint8_t> &r : *rows)
      got.push_back(TextValue(r));
    std::sort(got.begin(), got.end());
    return std::make_pair(got, collided);
  };

  // LIMIT counts rows, not buckets: a wide LIMIT returns every row sharing the
  // key, with no collisions at the default bucket count.
  auto [all, all_collided] = query("SELECT v FROM t WHERE k = :k LIMIT 16");
  EXPECT_EQ(all_collided, 0u);
  EXPECT_EQ(all, (std::vector<std::string>{"dup-0", "dup-1", "dup-2", "dup-3",
                                           "dup-4"}))
      << "all five rows with key 7 must come back";

  // The default (no LIMIT) returns up to kDefaultSelectLimit (10) clean rows,
  // not a self collision, so all five rows sharing the key come back.
  auto [one, one_collided] = query("SELECT v FROM t WHERE k = :k");
  EXPECT_EQ(one_collided, 0u);
  EXPECT_EQ(one.size(), 5u) << "default LIMIT returns the matching rows";

  // LIMIT 2 returns two rows; LIMIT 3 returns three. They are a stable prefix
  // of the full result, so the counts line up.
  EXPECT_EQ(query("SELECT v FROM t WHERE k = :k LIMIT 2").first.size(), 2u);
  EXPECT_EQ(query("SELECT v FROM t WHERE k = :k LIMIT 3").first.size(), 3u);

  // OFFSET pages through the rows: LIMIT 2 then LIMIT 2 OFFSET 2 are disjoint
  // and together with OFFSET 4 cover all five exactly once.
  std::vector<std::string> paged;
  for (const char *sql : {"SELECT v FROM t WHERE k = :k LIMIT 2",
                          "SELECT v FROM t WHERE k = :k LIMIT 2 OFFSET 2",
                          "SELECT v FROM t WHERE k = :k LIMIT 2 OFFSET 4"}) {
    for (const std::string &v : query(sql).first)
      paged.push_back(v);
  }
  std::sort(paged.begin(), paged.end());
  EXPECT_EQ(paged, (std::vector<std::string>{"dup-0", "dup-1", "dup-2", "dup-3",
                                             "dup-4"}))
      << "paging by OFFSET covers every row once";

  (*node)->Shutdown();
}

// A SELECT with no WHERE is a plaintext full scan: every row comes back, the
// row count is exact, and max_rows bounds how many rows the server returns.
TEST_F(ServerEndToEnd, ScanReturnsRowsWithNoWhere) {
  LoadTable(8); // keys 0..7, values value-0..value-7

  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  auto client = QueryClient::Create(cfg_, (*node)->listen_address());
  ASSERT_TRUE(client.ok()) << client.status().message();
  ASSERT_TRUE((*client)->Register("c1").ok());

  auto all = (*client)->Scan("default", "t", /*max_rows=*/100);
  ASSERT_TRUE(all.ok()) << all.status().message();
  EXPECT_EQ(all->total_rows, 8u);
  ASSERT_EQ(all->rows.size(), 8u);
  std::vector<std::string> got;
  for (const std::vector<std::uint8_t> &r : all->rows)
    got.push_back(TextValue(r));
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, (std::vector<std::string>{"value-0", "value-1", "value-2",
                                           "value-3", "value-4", "value-5",
                                           "value-6", "value-7"}));

  // max_rows bounds the rows returned but not the reported total.
  auto few = (*client)->Scan("default", "t", /*max_rows=*/3);
  ASSERT_TRUE(few.ok()) << few.status().message();
  EXPECT_EQ(few->rows.size(), 3u);
  EXPECT_EQ(few->total_rows, 8u);

  // max_rows 0 returns the count alone, the no-WHERE COUNT(*) path.
  auto count = (*client)->Scan("default", "t", /*max_rows=*/0);
  ASSERT_TRUE(count.ok()) << count.status().message();
  EXPECT_TRUE(count->rows.empty());
  EXPECT_EQ(count->total_rows, 8u);

  (*node)->Shutdown();
}

// WHERE k <> :k returns every row except the matched one, and COUNT(*) under
// '<>' is the exact non-matching count.
TEST_F(ServerEndToEnd, InequalityReturnsOtherRows) {
  LoadTable(8); // keys 0..7 unique

  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  auto client = QueryClient::Create(cfg_, (*node)->listen_address());
  ASSERT_TRUE(client.ok()) << client.status().message();
  ASSERT_TRUE((*client)->Register("c1").ok());

  // Exact count is robust to bucket collisions (the presence sum is exact).
  auto n =
      (*client)->QueryCount("c1", "SELECT COUNT(*) FROM t WHERE k <> :k", 3);
  ASSERT_TRUE(n.ok()) << n.status().message();
  EXPECT_EQ(*n, 7u) << "seven of eight rows have k <> 3";

  std::uint32_t collided = 0;
  auto rows = (*client)->Query("c1", "SELECT v FROM t WHERE k <> :k LIMIT 16",
                               3, "", "default", &collided);
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  for (const std::vector<std::uint8_t> &r : *rows) {
    const std::string v = TextValue(r);
    EXPECT_NE(v, "value-3") << "the matched row must be excluded";
    EXPECT_EQ(v.rfind("value-", 0), 0u);
  }

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

// A table with a primary key and a secondary INDEX column can be queried by
// either. A query on the secondary column matches its own sub-key inside the
// packed match record while the matcher stays single-key, and the row comes
// back including the index column itself (it is payload, so it is returnable).
TEST_F(ServerEndToEnd, PrivateSelectOnSecondaryIndex) {
  const Schema users(
      "users", {Column{"id", ColumnEncoding::kEq, ColumnType::kInt},
                Column{"username", ColumnEncoding::kIndex, ColumnType::kText},
                Column{"email", ColumnEncoding::kRaw, ColumnType::kText}});
  std::vector<IngestRow> data = {
      IngestRow{Value{std::int64_t{1}},
                {Value{std::string("admin")}, Value{std::string("a@x")}}},
      IngestRow{Value{std::int64_t{2}},
                {Value{std::string("bob")}, Value{std::string("b@x")}}},
      IngestRow{Value{std::int64_t{3}},
                {Value{std::string("carol")}, Value{std::string("c@x")}}}};
  auto staging = BuildStagingFromRows(cfg_, users, data);
  ASSERT_TRUE(staging.ok()) << staging.status().message();
  auto repo = LocalEpochRepository::Open(cfg_.EpochRootFor("default", "users"));
  ASSERT_TRUE(repo.ok());
  ASSERT_TRUE((*repo)->Publish(*staging, 1).ok());

  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());
  auto client = QueryClient::Create(cfg_, (*node)->listen_address());
  ASSERT_TRUE(client.ok()) << client.status().message();
  ASSERT_TRUE((*client)->Register("c1").ok());

  // Query by the secondary index column. SELECT * returns the payload, which
  // holds the index column (username) and the raw column (email).
  auto by_name = (*client)->Query(
      "c1", "SELECT * FROM users WHERE username = \"bob\"", 0, "");
  ASSERT_TRUE(by_name.ok()) << by_name.status().message();
  ASSERT_EQ(by_name->size(), 1u);
  auto nv = opaquedb::core::DecodeRecord(users, (*by_name)[0]);
  ASSERT_TRUE(nv.ok()) << nv.status().message();
  ASSERT_EQ(nv->size(), 2u);
  EXPECT_EQ(std::get<std::string>((*nv)[0]), "bob");
  EXPECT_EQ(std::get<std::string>((*nv)[1]), "b@x");

  // The same table is still queryable by its primary key.
  auto by_id =
      (*client)->Query("c1", "SELECT email FROM users WHERE id = :id", 3, "");
  ASSERT_TRUE(by_id.ok()) << by_id.status().message();
  ASSERT_EQ(by_id->size(), 1u);
  auto iv = opaquedb::core::DecodeRecord(users, (*by_id)[0]);
  ASSERT_TRUE(iv.ok()) << iv.status().message();
  EXPECT_EQ(std::get<std::string>((*iv)[1]), "c@x");

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

// With auth disabled, every caller is the anonymous principal with the Query
// role. That is enough to register keys and run a query, but it must NOT reach
// any Admin RPC: open mode is "no authentication", not "everyone is admin".
TEST_F(ServerEndToEnd, NoAuthHasQueryRoleButCannotAdminister) {
  // The fixture already runs auth.mode=none.
  LoadTable(8);
  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  // Query role works: register and run a private query with no token.
  auto client = QueryClient::Create(cfg_, (*node)->listen_address());
  ASSERT_TRUE(client.ok()) << client.status().message();
  ASSERT_TRUE((*client)->Register("c1").ok());
  auto rows = (*client)->Query("c1", "SELECT v FROM t WHERE k = :k", 2, "");
  ASSERT_TRUE(rows.ok()) << rows.status().message();

  auto channel = grpc::CreateChannel((*node)->listen_address(),
                                     grpc::InsecureChannelCredentials());

  // An Admin RPC is refused even with auth off: the anonymous principal has the
  // Query role, not Admin.
  auto admin_stub = opaquedb::proto::Admin::NewStub(channel);
  grpc::ClientContext sctx;
  opaquedb::proto::StatusRequest sreq;
  sreq.set_wire_version(opaquedb::core::kWireVersion);
  opaquedb::proto::StatusReply srep;
  EXPECT_EQ(admin_stub->Status(&sctx, sreq, &srep).error_code(),
            grpc::StatusCode::PERMISSION_DENIED);

  // Insert mutates data and also requires Admin, so it is refused too.
  auto query_stub = opaquedb::proto::OpaqueDB::NewStub(channel);
  grpc::ClientContext ictx;
  opaquedb::proto::InsertRequest ireq;
  ireq.set_wire_version(opaquedb::core::kWireVersion);
  ireq.set_table("t");
  *ireq.add_values() = "9";
  *ireq.add_values() = "nine";
  opaquedb::proto::InsertReply irep;
  EXPECT_EQ(query_stub->Insert(&ictx, ireq, &irep).error_code(),
            grpc::StatusCode::PERMISSION_DENIED);

  (*node)->Shutdown();
}

// Exhaustive role gate over every privileged RPC: in token mode an admin token
// is admitted, a query token is forbidden, and no token is unauthenticated.
// This is the "no-auth people cannot do anything" guarantee, asserted per RPC.
TEST_F(ServerEndToEnd, EveryPrivilegedRpcEnforcesItsRole) {
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
  auto admin = opaquedb::proto::Admin::NewStub(channel);
  auto query = opaquedb::proto::OpaqueDB::NewStub(channel);
  const std::uint32_t wire = opaquedb::core::kWireVersion;

  auto meta = [](grpc::ClientContext &ctx, const std::string &token) {
    if (!token.empty())
      ctx.AddMetadata("authorization", "Bearer " + token);
  };

  // One closure per privileged RPC, each returning the gRPC status code for a
  // given bearer token. Payloads are minimal: the gate runs before the handler
  // body, so the role decision does not depend on a fully valid request.
  std::vector<std::pair<std::string,
                        std::function<grpc::StatusCode(const std::string &)>>>
      rpcs;
  rpcs.emplace_back("Admin.Status", [&](const std::string &t) {
    grpc::ClientContext c;
    meta(c, t);
    opaquedb::proto::StatusRequest req;
    req.set_wire_version(wire);
    opaquedb::proto::StatusReply rep;
    return admin->Status(&c, req, &rep).error_code();
  });
  rpcs.emplace_back("Admin.InspectSchema", [&](const std::string &t) {
    grpc::ClientContext c;
    meta(c, t);
    opaquedb::proto::SchemaRequest req;
    req.set_wire_version(wire);
    opaquedb::proto::SchemaReply rep;
    return admin->InspectSchema(&c, req, &rep).error_code();
  });
  rpcs.emplace_back("Admin.ListEpochs", [&](const std::string &t) {
    grpc::ClientContext c;
    meta(c, t);
    opaquedb::proto::ListEpochsRequest req;
    req.set_wire_version(wire);
    opaquedb::proto::ListEpochsReply rep;
    return admin->ListEpochs(&c, req, &rep).error_code();
  });
  rpcs.emplace_back("Admin.RollbackEpoch", [&](const std::string &t) {
    grpc::ClientContext c;
    meta(c, t);
    opaquedb::proto::RollbackRequest req;
    req.set_wire_version(wire);
    req.set_version(1);
    opaquedb::proto::RollbackReply rep;
    return admin->RollbackEpoch(&c, req, &rep).error_code();
  });
  rpcs.emplace_back("Admin.ListPrincipals", [&](const std::string &t) {
    grpc::ClientContext c;
    meta(c, t);
    opaquedb::proto::PrincipalsRequest req;
    req.set_wire_version(wire);
    opaquedb::proto::PrincipalsReply rep;
    return admin->ListPrincipals(&c, req, &rep).error_code();
  });
  rpcs.emplace_back("Admin.Load", [&](const std::string &t) {
    grpc::ClientContext c;
    meta(c, t);
    opaquedb::proto::LoadReply rep;
    auto writer = admin->Load(&c, &rep);
    opaquedb::proto::LoadChunk chunk;
    chunk.set_wire_version(wire);
    writer->Write(chunk);
    writer->WritesDone();
    return writer->Finish().error_code();
  });
  rpcs.emplace_back("OpaqueDB.Insert", [&](const std::string &t) {
    grpc::ClientContext c;
    meta(c, t);
    opaquedb::proto::InsertRequest req;
    req.set_wire_version(wire);
    req.set_table("t");
    *req.add_values() = "9";
    *req.add_values() = "nine";
    opaquedb::proto::InsertReply rep;
    return query->Insert(&c, req, &rep).error_code();
  });

  for (const auto &[name, call] : rpcs) {
    EXPECT_EQ(call("q-token"), grpc::StatusCode::PERMISSION_DENIED)
        << name << " must reject the query role";
    EXPECT_EQ(call(""), grpc::StatusCode::UNAUTHENTICATED)
        << name << " must reject a missing token";
    const grpc::StatusCode admitted = call("a-token");
    EXPECT_NE(admitted, grpc::StatusCode::PERMISSION_DENIED)
        << name << " must admit the admin role";
    EXPECT_NE(admitted, grpc::StatusCode::UNAUTHENTICATED)
        << name << " must admit the admin role";
  }

  (*node)->Shutdown();
}

// Register once, then reuse the persisted keyset on a fresh node over the same
// data directory without registering again. This is the whole point of the
// file-backed keyring (server side) plus SerializeKeyset/CreateWithKeyset
// (client side): keys survive a restart and the client keeps its identity.
TEST_F(ServerEndToEnd, RegisterOnceSurvivesRestart) {
  LoadTable(8);

  std::string keyset;
  {
    auto node = NodeServer::Create(cfg_);
    ASSERT_TRUE(node.ok());
    ASSERT_TRUE((*node)->Start().ok());
    ASSERT_TRUE((*node)->WaitForReady().ok());

    auto client = QueryClient::Create(cfg_, (*node)->listen_address());
    ASSERT_TRUE(client.ok()) << client.status().message();
    ASSERT_TRUE((*client)->Register("c1").ok());
    auto rows = (*client)->Query("c1", "SELECT v FROM t WHERE k = :k", 5, "");
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    ASSERT_EQ(rows->size(), 1u);

    auto saved = (*client)->SerializeKeyset();
    ASSERT_TRUE(saved.ok()) << saved.status().message();
    keyset = *saved;
    (*node)->Shutdown();
  }

  // A brand new node over the same data_dir: the persisted keyring reloads c1's
  // keys from disk, and the client rebuilt from the saved keyset queries with
  // no second Register.
  auto node = NodeServer::Create(cfg_);
  ASSERT_TRUE(node.ok());
  ASSERT_TRUE((*node)->Start().ok());
  ASSERT_TRUE((*node)->WaitForReady().ok());

  auto client =
      QueryClient::CreateWithKeyset(cfg_, (*node)->listen_address(), keyset);
  ASSERT_TRUE(client.ok()) << client.status().message();
  auto rows = (*client)->Query("c1", "SELECT v FROM t WHERE k = :k", 5, "");
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ(TextValue((*rows)[0]), "value-5");

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

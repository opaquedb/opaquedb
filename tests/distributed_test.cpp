#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "grpcpp/grpcpp.h"
#include "opaquedb/client/query_client.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/core/slot_codec.h"
#include "opaquedb/crypto/context.h"
#include "opaquedb/crypto/key_material.h"
#include "opaquedb/crypto/ops.h"
#include "opaquedb/crypto/serialize.h"
#include "opaquedb/server/ingest.h"
#include "opaquedb/server/node_server.h"
#include "opaquedb/server/query_coordinator.h"
#include "opaquedb/storage/repository.h"

namespace {

namespace fs = std::filesystem;
using opaquedb::config::Config;
using opaquedb::core::Column;
using opaquedb::core::ColumnEncoding;
using opaquedb::core::ColumnType;
using opaquedb::core::Schema;
using opaquedb::core::Value;
using opaquedb::crypto::ClientKeyring;
using opaquedb::crypto::CryptoContext;
using opaquedb::server::BuildStagingFromRows;
using opaquedb::server::Engine;
using opaquedb::server::IngestRow;
using opaquedb::server::NodeServer;
using opaquedb::server::QueryCoordinator;
using opaquedb::storage::LocalEpochRepository;

// The shard fixtures use an integer key and one text value column.
Schema KvSchema() {
  return Schema("t", {Column{"k", ColumnEncoding::kEq, ColumnType::kInt},
                      Column{"v", ColumnEncoding::kRaw, ColumnType::kText}});
}

// Decodes the single text payload column from a returned record.
std::string TextOf(const Schema &schema,
                   const std::vector<std::uint8_t> &record) {
  auto values = opaquedb::core::DecodeRecord(schema, record);
  if (!values.ok() || values->empty())
    return "";
  return opaquedb::core::ValueToString(values->front());
}

// A two-node cluster with disjoint shards proves the distributed query path:
// the coordinator evaluates its own shard, fans the encrypted query to the
// peer, and combines the encrypted partials so the right row comes back even
// though no node holds the whole table.
class DistributedQuery : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("opaquedb_dist_" +
             std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(root_);
    for (Config *cfg : {&cfg_a_, &cfg_b_, &cfg_c_}) {
      cfg->storage.record_bytes = 32;
      cfg->server.listen = "127.0.0.1:0";
      cfg->auth.mode = opaquedb::config::AuthMode::kNone;
      cfg->auth.enable_insecure = true;
    }
    cfg_a_.node.data_dir = (root_ / "a").string();
    cfg_b_.node.data_dir = (root_ / "b").string();
    cfg_c_.node.data_dir = (root_ / "c").string();
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  void Publish(const Config &cfg,
               const std::vector<std::pair<std::uint64_t, std::string>> &kv) {
    std::vector<IngestRow> rows;
    rows.reserve(kv.size());
    for (const auto &[key, text] : kv)
      rows.push_back(
          IngestRow{Value{static_cast<std::int64_t>(key)}, {Value{text}}});
    auto staging = BuildStagingFromRows(cfg, KvSchema(), rows);
    ASSERT_TRUE(staging.ok()) << staging.status().message();
    auto repo = LocalEpochRepository::Open(cfg.EpochRootFor("default", "t"));
    ASSERT_TRUE(repo.ok());
    ASSERT_TRUE((*repo)->Publish(*staging, 1).ok());
  }

  // Encodes the WHERE value into the serialized encrypted query the engine
  // expects, the same way the client does.
  std::string EncodeQuery(const CryptoContext &ctx, const ClientKeyring &keys,
                          std::uint64_t value) {
    auto enc = opaquedb::crypto::BuildEncryptedOperand(
        ctx, keys.public_key(), value, cfg_a_.crypto.key_bits);
    EXPECT_TRUE(enc.ok()) << enc.status().message();
    return *enc;
  }

  std::string DecodeResult(const CryptoContext &ctx, const ClientKeyring &keys,
                           const std::string &blob) {
    // The engine partitions matches into result_buckets buckets, so decode the
    // whole partition (as the real client does) and return the one clean row.
    // These tests use unique keys, so there is at most one match.
    auto recs = opaquedb::crypto::DecryptResults(
        ctx, keys.secret_key(), blob, cfg_a_.crypto.key_bits,
        cfg_a_.storage.record_bytes, cfg_a_.crypto.BytesPerSlot(),
        cfg_a_.crypto.result_buckets, /*offset=*/0,
        /*limit=*/cfg_a_.crypto.result_buckets);
    EXPECT_TRUE(recs.ok()) << recs.status().message();
    for (const opaquedb::crypto::BucketResult &br : *recs) {
      if (br.present && !br.collided)
        return TextOf(KvSchema(), br.record);
    }
    return "";
  }

  Config cfg_a_;
  Config cfg_b_;
  Config cfg_c_;
  fs::path root_;
};

TEST_F(DistributedQuery, FansOutAndCombinesAcrossShards) {
  // Even keys live on node A, odd keys on node B: disjoint shards.
  Publish(cfg_a_, {{0, "value-0"}, {2, "value-2"}, {4, "value-4"}});
  Publish(cfg_b_, {{1, "value-1"}, {3, "value-3"}, {5, "value-5"}});

  auto node_a = NodeServer::Create(cfg_a_);
  auto node_b = NodeServer::Create(cfg_b_);
  ASSERT_TRUE(node_a.ok());
  ASSERT_TRUE(node_b.ok());
  ASSERT_TRUE((*node_a)->Start().ok());
  ASSERT_TRUE((*node_b)->Start().ok());
  ASSERT_TRUE((*node_a)->WaitForReady().ok());
  ASSERT_TRUE((*node_b)->WaitForReady().ok());

  // One client keypair, registered on both nodes (the BlobStore distributes
  // keys in production; here we register directly).
  auto ctx = CryptoContext::Create(cfg_a_.crypto);
  ASSERT_TRUE(ctx.ok());
  ClientKeyring keys = ClientKeyring::Generate(*ctx);
  auto material = keys.SerializePublic();
  ASSERT_TRUE(material.ok());
  ASSERT_TRUE((*node_a)->engine()->RegisterClient("c1", *material).ok());
  ASSERT_TRUE((*node_b)->engine()->RegisterClient("c1", *material).ok());

  // A coordinates; its peer is B. Raise the channel message size for the
  // encrypted query, which is several megabytes.
  grpc::ChannelArguments args;
  args.SetMaxReceiveMessageSize(64 * 1024 * 1024);
  args.SetMaxSendMessageSize(64 * 1024 * 1024);
  auto peer = grpc::CreateCustomChannel(
      (*node_b)->listen_address(), grpc::InsecureChannelCredentials(), args);
  QueryCoordinator coordinator((*node_a)->engine(), {peer}, /*epoch=*/1,
                               64 * 1024 * 1024);

  const std::string sql = "SELECT v FROM t WHERE k = :k";

  // Key 3 is on the peer shard (B): the fan-out plus combine must find it.
  std::string q3 = EncodeQuery(*ctx, keys, 3);
  auto r3 = coordinator.Execute("c1", "default", sql, q3, "");
  ASSERT_TRUE(r3.ok()) << r3.status().message();
  ASSERT_EQ(r3->encrypted_results.size(), 1u);
  EXPECT_EQ(DecodeResult(*ctx, keys, r3->encrypted_results[0]), "value-3");

  // Key 4 is on the local shard (A).
  std::string q4 = EncodeQuery(*ctx, keys, 4);
  auto r4 = coordinator.Execute("c1", "default", sql, q4, "");
  ASSERT_TRUE(r4.ok()) << r4.status().message();
  EXPECT_EQ(DecodeResult(*ctx, keys, r4->encrypted_results[0]), "value-4");

  // A key in neither shard returns no data.
  std::string q9 = EncodeQuery(*ctx, keys, 99);
  auto r9 = coordinator.Execute("c1", "default", sql, q9, "");
  ASSERT_TRUE(r9.ok());
  EXPECT_TRUE(DecodeResult(*ctx, keys, r9->encrypted_results[0]).empty());

  (*node_a)->Shutdown();
  (*node_b)->Shutdown();
}

// The end-to-end goal: a client connects to one node, registers once, and runs
// a normal query. That node coordinates, every shard does its part, and the
// combined row comes back, even though the client talks to only one node.
TEST_F(DistributedQuery, ClientQueryThroughCoordinatorFansOutToAllShards) {
  // Three disjoint shards across three nodes.
  Publish(cfg_a_, {{0, "value-0"}, {3, "value-3"}});
  Publish(cfg_b_, {{1, "value-1"}, {4, "value-4"}});
  Publish(cfg_c_, {{2, "value-2"}, {5, "value-5"}});

  auto node_a = NodeServer::Create(cfg_a_);
  auto node_b = NodeServer::Create(cfg_b_);
  auto node_c = NodeServer::Create(cfg_c_);
  ASSERT_TRUE(node_a.ok() && node_b.ok() && node_c.ok());
  ASSERT_TRUE((*node_a)->Start().ok());
  ASSERT_TRUE((*node_b)->Start().ok());
  ASSERT_TRUE((*node_a)->WaitForReady().ok());
  ASSERT_TRUE((*node_b)->WaitForReady().ok());
  ASSERT_TRUE((*node_c)->Start().ok());
  ASSERT_TRUE((*node_a)->WaitForReady().ok());
  ASSERT_TRUE((*node_b)->WaitForReady().ok());
  ASSERT_TRUE((*node_c)->WaitForReady().ok());

  // Node A coordinates; B and C are its shard peers.
  (*node_a)->SetQueryPeers(
      {(*node_b)->listen_address(), (*node_c)->listen_address()});

  // The client talks only to A. Registering forwards its keys to B and C so
  // they can evaluate.
  auto client = opaquedb::client::QueryClient::Create(
      cfg_a_, (*node_a)->listen_address());
  ASSERT_TRUE(client.ok()) << client.status().message();
  ASSERT_TRUE((*client)->Register("c1").ok());

  const std::string sql = "SELECT v FROM t WHERE k = :k";
  auto value_of = [&](std::uint64_t key) -> std::string {
    auto rows = (*client)->Query("c1", sql, key, "");
    EXPECT_TRUE(rows.ok()) << rows.status().message();
    if (!rows.ok())
      return "<error>";
    if (rows->empty())
      return ""; // no row matched (presence indicator was zero)
    if (rows->size() != 1)
      return "<error>";
    return TextOf(KvSchema(), (*rows)[0]);
  };

  EXPECT_EQ(value_of(0), "value-0"); // local shard (A)
  EXPECT_EQ(value_of(4), "value-4"); // peer shard (B)
  EXPECT_EQ(value_of(2), "value-2"); // peer shard (C)
  EXPECT_EQ(value_of(5), "value-5"); // peer shard (C)
  EXPECT_TRUE(value_of(99).empty()); // absent everywhere

  (*node_a)->Shutdown();
  (*node_b)->Shutdown();
  (*node_c)->Shutdown();
}

} // namespace

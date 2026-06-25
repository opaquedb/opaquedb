// examples/direct_engine.cpp
// Drive the Engine in-process, with no gRPC and no network. It declares a
// weather table, loads a few rows into an epoch, runs a private SELECT by
// calling the engine directly, then decrypts and decodes the typed result. This
// is the shortest path that exercises the SQL, crypto, storage, and PIR layers
// together.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "opaquedb/admin/keyring_store.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/key_codec.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/core/slot_codec.h"
#include "opaquedb/crypto/context.h"
#include "opaquedb/crypto/key_material.h"
#include "opaquedb/crypto/ops.h"
#include "opaquedb/server/engine.h"
#include "opaquedb/server/ingest.h"
#include "opaquedb/sql/ddl.h"
#include "opaquedb/storage/repository.h"

// Pull in the reference backend registration (normally done by NodeServer).
#include "opaquedb/backend/reference/reference.h"

namespace fs = std::filesystem;
using opaquedb::admin::InMemoryKeyringStore;
using opaquedb::config::Config;
using opaquedb::core::Value;
using opaquedb::crypto::ClientKeyring;
using opaquedb::crypto::CryptoContext;
using opaquedb::server::BuildStagingFromRows;
using opaquedb::server::Engine;
using opaquedb::server::IngestRow;
using opaquedb::storage::LocalEpochRepository;

namespace {

const char *kSchema = R"sql(
CREATE TABLE weather (
  city TEXT KEY,
  id INT,
  country TEXT,
  temperature INT
);
)sql";

// key: city. payload in column order: id, country, temperature.
std::vector<IngestRow> WeatherRows() {
  return {
      {std::string("Amsterdam"),
       {std::int64_t{1}, std::string("NL"), std::int64_t{18}}},
      {std::string("Tokyo"),
       {std::int64_t{2}, std::string("JP"), std::int64_t{27}}},
      {std::string("Nairobi"),
       {std::int64_t{3}, std::string("KE"), std::int64_t{24}}},
      {std::string("Reykjavik"),
       {std::int64_t{4}, std::string("IS"), std::int64_t{9}}},
  };
}

} // namespace

int main() {
  fs::path tmp = fs::temp_directory_path() / "opaquedb_direct_engine";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp);

  Config cfg;
  cfg.node.data_dir = tmp.string();
  cfg.storage.record_bytes = 64;
  cfg.auth.mode = opaquedb::config::AuthMode::kNone;
  cfg.auth.enable_insecure = true;

  auto schema = opaquedb::sql::ParseCreateTable(kSchema);
  if (!schema.ok()) {
    std::cerr << "schema: " << schema.status().message() << "\n";
    return 1;
  }
  auto staging = BuildStagingFromRows(cfg, *schema, WeatherRows());
  if (!staging.ok()) {
    std::cerr << "staging: " << staging.status().message() << "\n";
    return 1;
  }
  auto repo = LocalEpochRepository::Open(cfg.EffectiveEpochDir());
  if (!repo.ok() || !(*repo)->Publish(*staging, 1).ok()) {
    std::cerr << "publish failed\n";
    return 1;
  }

  opaquedb::backend::reference::LinkReferenceBackend();

  InMemoryKeyringStore keyring_store;
  auto engine = Engine::Create(cfg.crypto, repo->get(), &keyring_store);
  if (!engine.ok()) {
    std::cerr << "engine: " << engine.status().message() << "\n";
    return 1;
  }

  // Client side: generate keys and register the public set with the node.
  auto client_ctx = CryptoContext::Create(cfg.crypto);
  if (!client_ctx.ok()) {
    std::cerr << client_ctx.status().message() << "\n";
    return 1;
  }
  ClientKeyring kr = ClientKeyring::Generate(
      *client_ctx, opaquedb::core::RequiredGaloisSteps(
                       static_cast<std::uint32_t>(client_ctx->slot_count()),
                       cfg.crypto.key_bits));
  auto mat = kr.SerializePublic();
  if (!mat.ok() || !(*engine)->RegisterClient("direct", *mat).ok()) {
    std::cerr << "register keys failed\n";
    return 1;
  }

  // Encode the TEXT key to its matcher integer, then encrypt: bit-expand it and
  // tile it across the slots.
  const std::string want_city = "Amsterdam";
  auto key_value = opaquedb::core::EncodeKeyValue(
      opaquedb::core::ColumnType::kText, Value{want_city}, cfg.crypto.key_bits);
  if (!key_value.ok()) {
    std::cerr << key_value.status().message() << "\n";
    return 1;
  }
  auto enc = opaquedb::crypto::BuildEncryptedOperand(
      *client_ctx, kr.public_key(), key_value->value, cfg.crypto.key_bits);
  if (!enc.ok()) {
    std::cerr << enc.status().message() << "\n";
    return 1;
  }

  auto res = (*engine)->Execute(
      "direct", "default",
      "SELECT id, country, temperature FROM weather WHERE city = :city", *enc,
      "");
  if (!res.ok()) {
    std::cerr << "execute: " << res.status().message() << "\n";
    return 1;
  }

  const std::uint32_t bps = cfg.crypto.BytesPerSlot();
  for (const auto &blob : res->encrypted_results) {
    auto bytes = opaquedb::crypto::DecryptRecord(*client_ctx, kr.secret_key(),
                                                 blob, cfg.crypto.key_bits,
                                                 cfg.storage.record_bytes, bps);
    if (!bytes.ok()) {
      std::cerr << "decrypt: " << bytes.status().message() << "\n";
      return 1;
    }
    if (!bytes->has_value()) {
      std::cout << want_city << " -> no match\n";
      continue;
    }
    auto values = opaquedb::core::DecodeRecord(*schema, **bytes);
    if (!values.ok()) {
      std::cerr << "decode: " << values.status().message() << "\n";
      return 1;
    }
    std::cout << want_city << " ->";
    for (const Value &v : *values)
      std::cout << " " << opaquedb::core::ValueToString(v);
    std::cout << "\n";
  }

  fs::remove_all(tmp, ec);
  return 0;
}

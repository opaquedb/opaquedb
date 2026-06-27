// examples/e2e_client.cpp
// A standalone gRPC client built on the QueryClient library. It generates a
// keypair, registers the public keys, encrypts the WHERE value, runs Execute,
// and decrypts the result. Pass --schema to decode the typed columns; without
// it the raw record bytes are printed.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "CLI/CLI.hpp"
#include "absl/status/status.h"
#include "opaquedb/client/query_client.h"
#include "opaquedb/config/config.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/sql/ddl.h"

namespace {

using opaquedb::config::Config;
using opaquedb::core::Column;
using opaquedb::core::ColumnEncoding;
using opaquedb::core::Schema;
using opaquedb::core::Value;

std::string DialTarget(const std::string &listen) {
  std::string t = listen;
  if (t.find("0.0.0.0") != std::string::npos)
    t.replace(t.find("0.0.0.0"), 7, "127.0.0.1");
  if (t.find("[::]") != std::string::npos)
    t.replace(t.find("[::]"), 4, "[::1]");
  return t;
}

std::string ReadFile(const std::string &path) {
  std::ifstream in(path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

} // namespace

int main(int argc, char **argv) {
  CLI::App app{"OpaqueDB end-to-end client demo"};
  std::string target = "127.0.0.1:50051";
  std::string client_id = "demo";
  std::string database = "default";
  std::string sql =
      "SELECT country, temperature, conditions FROM weather WHERE city = "
      "\"Amsterdam\"";
  std::string schema_path;
  std::uint64_t param = 0;

  app.add_option("--target", target, "host:port of a node");
  app.add_option("--client-id", client_id, "Client id");
  app.add_option("--database", database, "Database holding the table");
  app.add_option("--sql", sql, "SQL template or query with an inline literal");
  app.add_option("--schema", schema_path,
                 "CREATE TABLE schema file, to decode typed results");
  app.add_option("--param", param,
                 "Bound WHERE value (omit when the SQL has a literal)");
  CLI11_PARSE(app, argc, argv);

  target = DialTarget(target);

  Config
      cfg; // crypto and storage defaults, matching a node started the same way
  // This example talks to a local dev node over a plaintext channel. A real
  // client sets client.ca_cert (and client.tls_cert/tls_key for mutual TLS).
  cfg.client.allow_insecure = true;

  Schema schema;
  bool have_schema = false;
  if (!schema_path.empty()) {
    auto parsed = opaquedb::sql::ParseCreateTable(ReadFile(schema_path));
    if (!parsed.ok()) {
      std::cerr << "schema: " << parsed.status().message() << "\n";
      return 1;
    }
    schema = *parsed;
    have_schema = true;
  }

  auto c = opaquedb::client::QueryClient::Create(cfg, target, "");
  if (!c.ok()) {
    std::cerr << "create client: " << c.status().message() << "\n";
    return 1;
  }
  if (absl::Status s = (*c)->Register(client_id); !s.ok()) {
    std::cerr << "register: " << s.message() << "\n";
    return 1;
  }
  auto rows = (*c)->Query(client_id, sql, param, "", database);
  if (!rows.ok()) {
    std::cerr << "query: " << rows.status().message() << "\n";
    return 1;
  }
  if (rows->empty()) {
    std::cout << "(no rows)\n";
    return 0;
  }
  for (const std::vector<std::uint8_t> &row : *rows) {
    if (have_schema) {
      auto values = opaquedb::core::DecodeRecord(schema, row);
      if (!values.ok()) {
        std::cerr << "decode: " << values.status().message() << "\n";
        return 1;
      }
      std::size_t i = 0;
      for (const Column &col : schema.columns()) {
        if (col.encoding == ColumnEncoding::kEq || i >= values->size())
          continue;
        std::cout << col.name << "="
                  << opaquedb::core::ValueToString((*values)[i++]) << " ";
      }
      std::cout << "\n";
    } else {
      std::string v(row.begin(), row.end());
      while (!v.empty() && v.back() == '\0')
        v.pop_back();
      std::cout << v << "\n";
    }
  }
  return 0;
}

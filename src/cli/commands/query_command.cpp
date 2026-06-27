#include "query_command.h"

#include <iostream>
#include <string>
#include <vector>

#include "../util.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "opaquedb/client/query_client.h"
#include "opaquedb/core/record_codec.h"
#include "opaquedb/core/schema.h"
#include "opaquedb/sql/ddl.h"
#include "opaquedb/sql/parser.h"
#include "spdlog/spdlog.h"

namespace opaquedb::cli {
namespace {

// Turns a listen address into one a local client can dial: a wildcard bind
// address is not connectable, so map it to loopback.
std::string DialTarget(const std::string &listen) {
  return absl::StrReplaceAll(listen,
                             {{"0.0.0.0", "127.0.0.1"}, {"[::]", "[::1]"}});
}

} // namespace

void QueryCommand::Register(CLI::App &parent, const GlobalOptions &globals,
                            int &exit_code) {
  CLI::App *query =
      parent.add_subcommand("query", "Dev-only: run a private SELECT");
  query
      ->add_option("sql", sql_,
                   "SQL template, e.g. SELECT city FROM weather WHERE id = :id")
      ->required();
  query->add_option("--param", value_,
                    "Integer WHERE value to encrypt (omit when the SQL has an "
                    "inline literal)");
  query->add_option("--database", database_,
                    "Database holding the table (default \"default\")");
  query->add_option("--target", target_, "host:port (default: from config)");
  query->add_option("--client-id", client_id_, "Client id")->default_val("dev");
  query->add_option("--backend", backend_,
                    "Backend name (default: by capability)");
  query->add_option("--token", token_, "Bearer token for token auth mode");
  query->add_option("--schema", schema_,
                    "CREATE TABLE schema file, to decode typed results");
  query->callback([this, &globals, &exit_code]() {
    absl::StatusOr<config::Config> config = LoadConfig(globals);
    if (!config.ok()) {
      spdlog::error("query: {}", config.status().message());
      exit_code = 1;
      return;
    }

    core::Schema schema;
    bool have_schema = false;
    if (!schema_.empty()) {
      absl::StatusOr<std::string> ddl = ReadFile(schema_);
      if (!ddl.ok()) {
        spdlog::error("query: {}", ddl.status().message());
        exit_code = 1;
        return;
      }
      absl::StatusOr<core::Schema> parsed = sql::ParseCreateTable(*ddl);
      if (!parsed.ok()) {
        spdlog::error("query: schema: {}", parsed.status().message());
        exit_code = 1;
        return;
      }
      schema = *std::move(parsed);
      have_schema = true;
    }

    const std::string target =
        target_.empty() ? DialTarget(config->server.listen) : target_;

    absl::StatusOr<std::unique_ptr<client::QueryClient>> client =
        client::QueryClient::Create(*config, target, token_);
    if (!client.ok()) {
      spdlog::error("query: {}", client.status().message());
      exit_code = 1;
      return;
    }
    if (absl::Status s = (*client)->Register(client_id_); !s.ok()) {
      spdlog::error("query: register failed: {}", s.message());
      exit_code = 1;
      return;
    }

    // Parse the SQL for the projection and table so results show only the
    // selected columns. If no schema file was given, fetch the table's schema
    // from the node so rows decode without --schema.
    std::vector<std::string> projection;
    bool count_star = false;
    if (absl::StatusOr<sql::SelectStatement> stmt = sql::Parse(sql_);
        stmt.ok()) {
      projection = stmt->projection;
      count_star = stmt->count_star;
      if (!have_schema) {
        if (absl::StatusOr<core::Schema> fetched =
                (*client)->DescribeTable(database_, stmt->table);
            fetched.ok()) {
          schema = *std::move(fetched);
          have_schema = true;
        }
      }
    }

    // COUNT(*) returns a single number from the encrypted match count, not
    // rows, so it has its own path and renders just the count.
    if (count_star) {
      absl::StatusOr<std::uint64_t> n =
          (*client)->QueryCount(client_id_, sql_, value_, backend_, database_);
      if (!n.ok()) {
        spdlog::error("query: {}", n.status().message());
        exit_code = 1;
        return;
      }
      std::cout << *n << "\n";
      return;
    }

    std::uint32_t collided = 0;
    absl::StatusOr<std::vector<std::vector<std::uint8_t>>> rows =
        (*client)->Query(client_id_, sql_, value_, backend_, database_,
                         &collided);
    if (!rows.ok()) {
      spdlog::error("query: {}", rows.status().message());
      exit_code = 1;
      return;
    }
    if (rows->empty()) {
      std::cout << "(no rows)\n";
    }
    for (const std::vector<std::uint8_t> &row : *rows) {
      std::cout << (have_schema ? RenderWithSchema(schema, row, projection)
                                : RenderRaw(row))
                << "\n";
    }
    if (collided > 0) {
      spdlog::warn("{} result bucket(s) dropped: multiple rows with the same "
                   "key collided; raise crypto.result_buckets or page with "
                   "OFFSET to recover them",
                   collided);
    }
  });
}

} // namespace opaquedb::cli

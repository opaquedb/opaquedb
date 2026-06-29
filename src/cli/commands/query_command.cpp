#include "query_command.h"

#include <iostream>
#include <string>

#include "../util.h"
#include "absl/strings/str_replace.h"
#include "opaquedb/client/query_client.h"
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
  CLI::Option *param_opt =
      query->add_option("--param", value_,
                        "Integer WHERE value to encrypt (omit when the SQL has "
                        "an inline literal)");
  query->add_option("--database", database_,
                    "Database holding the table (default \"default\")");
  query->add_option("--target", target_, "host:port (default: from config)");
  query->add_option("--client-id", client_id_, "Client id")->default_val("dev");
  query->add_option("--backend", backend_,
                    "Backend name (default: by capability)");
  query->add_option("--token", token_, "Bearer token for token auth mode");
  query->callback([this, &globals, &exit_code, param_opt]() {
    absl::StatusOr<config::Config> config = LoadConfig(globals);
    if (!config.ok()) {
      spdlog::error("query: {}", config.status().message());
      exit_code = 1;
      return;
    }

    // A query with a bound parameter (:name) and no inline literal needs
    // --param. Without this check the value would default to 0 and the query
    // would silently run for 0, returning wrong results with no error. Inline
    // literals (WHERE id = 42) carry their own value, so they need no --param.
    if (absl::StatusOr<sql::PreparedQuery> prepared =
            sql::PrepareClientQuery(sql_);
        prepared.ok() && prepared->literals.empty() &&
        prepared->sql_template.find(':') != std::string::npos &&
        param_opt->count() == 0) {
      spdlog::error("query: the SQL has a bound parameter but no --param was "
                    "given; pass --param <int> or use an inline literal");
      exit_code = 1;
      return;
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

    // One shared path handles COUNT(*), a full scan (no WHERE), and a matched
    // query, including DISTINCT / ORDER BY / LIMIT / OFFSET over decoded rows.
    if (absl::Status s =
            RunSelect(**client, client_id_, sql_, value_, backend_, database_,
                      std::cout, /*schema_cache=*/nullptr);
        !s.ok()) {
      spdlog::error("query: {}", s.message());
      exit_code = 1;
      return;
    }
  });
}

} // namespace opaquedb::cli

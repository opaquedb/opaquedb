#include "insert_command.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../util.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "opaquedb/client/query_client.h"
#include "spdlog/spdlog.h"

namespace opaquedb::cli {
namespace {

// A wildcard bind address is not connectable, so map it to loopback for a local
// client. Mirrors the query command.
std::string DialTarget(const std::string &listen) {
  return absl::StrReplaceAll(listen,
                             {{"0.0.0.0", "127.0.0.1"}, {"[::]", "[::1]"}});
}

} // namespace

void InsertCommand::Register(CLI::App &parent, const GlobalOptions &globals,
                             int &exit_code) {
  CLI::App *insert = parent.add_subcommand(
      "insert", "Insert one row into an existing table (plaintext)");
  insert->add_option("--table", table_, "Table to insert into")->required();
  insert->add_option("--database", database_,
                     "Database holding the table (default \"default\")");
  insert->add_option("--target", target_, "host:port (default: from config)");
  insert->add_option("--token", token_, "Bearer token for token auth mode");
  insert
      ->add_option("values", values_,
                   "Column values in CREATE TABLE order, including the key")
      ->required();
  insert->callback([this, &globals, &exit_code]() {
    absl::StatusOr<config::Config> config = LoadConfig(globals);
    if (!config.ok()) {
      spdlog::error("insert: {}", config.status().message());
      exit_code = 1;
      return;
    }

    const std::string target =
        target_.empty() ? DialTarget(config->server.listen) : target_;

    absl::StatusOr<std::unique_ptr<client::QueryClient>> client =
        client::QueryClient::Create(*config, target, token_);
    if (!client.ok()) {
      spdlog::error("insert: {}", client.status().message());
      exit_code = 1;
      return;
    }

    absl::StatusOr<client::QueryClient::InsertResult> result =
        (*client)->Insert(database_, table_, values_);
    if (!result.ok()) {
      spdlog::error("insert: {}", result.status().message());
      exit_code = 1;
      return;
    }
    std::cout << "inserted 1 row into " << database_ << "." << table_
              << " (epoch " << result->epoch_version << ", " << result->row_count
              << " rows total)\n";
  });
}

} // namespace opaquedb::cli

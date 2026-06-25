#include "repl_command.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <strings.h>
#include <vector>

#include "../util.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "opaquedb/client/query_client.h"
#include "opaquedb/sql/ddl.h"
#include "opaquedb/sql/parser.h"
#include "opaquedb/storage/catalog.h"
#include "replxx.hxx"
#include "spdlog/spdlog.h"

namespace opaquedb::cli {
namespace {

std::string DialTarget(const std::string &listen) {
  return absl::StrReplaceAll(listen,
                             {{"0.0.0.0", "127.0.0.1"}, {"[::]", "[::1]"}});
}

void PrintHelp() {
  std::cout << "commands:\n"
               "  <SELECT ...>        run a private query in the current "
               "database\n"
               "  \\use <database>     switch the current database\n"
               "  \\schema <file>      load a CREATE TABLE schema to decode "
               "rows\n"
               "  \\tables             list tables in the current database\n"
               "  \\help               show this help\n"
               "  \\quit               exit\n"
               "queries may use an inline literal, e.g.\n"
               "  SELECT temperature FROM weather WHERE city = \"London\"\n";
}

} // namespace

void ReplCommand::Register(CLI::App &parent, const GlobalOptions &globals,
                           int &exit_code) {
  CLI::App *repl =
      parent.add_subcommand("repl", "Interactive private-query shell");
  repl->add_option("--target", target_, "host:port (default: from config)");
  repl->add_option("--client-id", client_id_, "Client id")->default_val("repl");
  repl->add_option("--token", token_, "Bearer token for token auth mode");
  repl->add_option("--database", database_, "Initial database")
      ->default_val("default");
  repl->add_option("--schema", schema_,
                   "CREATE TABLE schema file, to decode typed results");
  repl->callback([this, &globals, &exit_code]() {
    absl::StatusOr<config::Config> config = LoadConfig(globals);
    if (!config.ok()) {
      spdlog::error("repl: {}", config.status().message());
      exit_code = 1;
      return;
    }

    core::Schema schema;
    bool have_schema = false;
    auto load_schema = [&](const std::string &path) -> bool {
      absl::StatusOr<std::string> ddl = ReadFile(path);
      if (!ddl.ok()) {
        std::cout << "schema: " << ddl.status().message() << "\n";
        return false;
      }
      absl::StatusOr<core::Schema> parsed = sql::ParseCreateTable(*ddl);
      if (!parsed.ok()) {
        std::cout << "schema: " << parsed.status().message() << "\n";
        return false;
      }
      schema = *std::move(parsed);
      have_schema = true;
      return true;
    };
    if (!schema_.empty())
      load_schema(schema_);

    const std::string target =
        target_.empty() ? DialTarget(config->server.listen) : target_;
    absl::StatusOr<std::unique_ptr<client::QueryClient>> client =
        client::QueryClient::Create(*config, target, token_);
    if (!client.ok()) {
      spdlog::error("repl: {}", client.status().message());
      exit_code = 1;
      return;
    }
    if (absl::Status s = (*client)->Register(client_id_); !s.ok()) {
      spdlog::error("repl: register failed: {}", s.message());
      exit_code = 1;
      return;
    }

    std::string database = database_;
    // Schemas fetched from the node, cached by "database.table", so rows decode
    // and project without the user supplying a schema file.
    std::map<std::string, core::Schema> schema_cache;

    // replxx gives line editing and history (up arrow recalls past lines) plus
    // tab completion of keywords and meta-commands.
    replxx::Replxx rx;
    rx.install_window_change_handler();
    rx.set_completion_callback([](const std::string &context,
                                  int &len) -> replxx::Replxx::completions_t {
      static const std::vector<std::string> kCandidates = {
          "SELECT", "FROM",     "WHERE",    "AND",    "OR",
          "\\use",  "\\schema", "\\tables", "\\help", "\\quit"};
      const std::size_t ws = context.find_last_of(" \t");
      const std::string prefix =
          context.substr(ws == std::string::npos ? 0 : ws + 1);
      len = static_cast<int>(prefix.size());
      replxx::Replxx::completions_t out;
      for (const std::string &cand : kCandidates) {
        if (cand.size() >= prefix.size() &&
            strncasecmp(cand.c_str(), prefix.c_str(), prefix.size()) == 0)
          out.emplace_back(cand);
      }
      return out;
    });
    std::cout << "OpaqueDB shell. \\help for commands, \\quit to exit.\n";

    while (true) {
      const std::string prompt = "opaquedb(" + database + ")> ";
      const char *cline = rx.input(prompt);
      if (cline == nullptr)
        break; // EOF (Ctrl-D) or interrupt
      std::string line(cline);
      std::string_view trimmed = absl::StripAsciiWhitespace(line);
      if (trimmed.empty())
        continue;
      rx.history_add(line);

      if (trimmed.front() == '\\') {
        const std::pair<std::string_view, std::string_view> parts =
            absl::StrSplit(trimmed, absl::MaxSplits(' ', 1));
        const std::string_view cmd = parts.first;
        const std::string_view arg = absl::StripAsciiWhitespace(parts.second);
        if (cmd == "\\quit" || cmd == "\\q") {
          break;
        } else if (cmd == "\\help" || cmd == "\\h") {
          PrintHelp();
        } else if (cmd == "\\use") {
          if (arg.empty()) {
            std::cout << "usage: \\use <database>\n";
          } else {
            // Validate against the catalog so switching to a database with no
            // tables is reported rather than silently accepted. (Local catalog;
            // the dev REPL is normally co-located with the node.)
            storage::Catalog catalog(config->DatabasesDir());
            absl::StatusOr<std::vector<std::string>> dbs =
                catalog.ListDatabases();
            if (dbs.ok() && std::find(dbs->begin(), dbs->end(),
                                      std::string(arg)) == dbs->end()) {
              std::cout << "error: no database '" << arg
                        << "' (load a table into it first)\n";
            } else {
              database = std::string(arg);
            }
          }
        } else if (cmd == "\\schema") {
          if (arg.empty())
            std::cout << "usage: \\schema <file>\n";
          else if (load_schema(std::string(arg)))
            std::cout << "decoding rows with schema for table '"
                      << schema.table() << "'\n";
        } else if (cmd == "\\tables") {
          // Tables in the current database, names only.
          storage::Catalog catalog(config->DatabasesDir());
          absl::StatusOr<std::vector<core::TableId>> ids = catalog.ListTables();
          if (!ids.ok()) {
            std::cout << ids.status().message() << "\n";
          } else {
            bool any = false;
            for (const core::TableId &id : *ids) {
              if (id.database != database)
                continue;
              std::cout << id.table << "\n";
              any = true;
            }
            if (!any)
              std::cout << "(no tables in '" << database << "')\n";
          }
        } else {
          std::cout << "unknown command '" << cmd << "'; \\help for the list\n";
        }
        continue;
      }

      // Parse the projection and table so results show only the selected
      // columns, and fetch (and cache) the table's schema to decode rows.
      std::vector<std::string> projection;
      const core::Schema *render_schema = have_schema ? &schema : nullptr;
      if (absl::StatusOr<sql::SelectStatement> stmt =
              sql::Parse(std::string(trimmed));
          stmt.ok()) {
        projection = stmt->projection;
        const std::string key = database + "." + stmt->table;
        auto it = schema_cache.find(key);
        if (it == schema_cache.end()) {
          if (absl::StatusOr<core::Schema> fetched =
                  (*client)->DescribeTable(database, stmt->table);
              fetched.ok()) {
            it = schema_cache.emplace(key, *std::move(fetched)).first;
          }
        }
        if (it != schema_cache.end())
          render_schema = &it->second;
      }

      absl::StatusOr<std::vector<std::vector<std::uint8_t>>> rows =
          (*client)->Query(client_id_, std::string(trimmed), /*value=*/0,
                           /*backend_hint=*/"", database);
      if (!rows.ok()) {
        std::cout << "error: " << rows.status().message() << "\n";
        continue;
      }
      if (rows->empty()) {
        std::cout << "(no rows)\n";
        continue;
      }
      for (const std::vector<std::uint8_t> &row : *rows) {
        std::cout << (render_schema
                          ? RenderWithSchema(*render_schema, row, projection)
                          : RenderRaw(row))
                  << "\n";
      }
    }
  });
}

} // namespace opaquedb::cli

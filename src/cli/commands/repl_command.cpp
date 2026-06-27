#include "repl_command.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
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
               "  <SELECT ...>;       run a query in the current database "
               "(end with ';')\n"
               "  \\use <database>     switch the current database\n"
               "  \\tables             list tables in the current database\n"
               "  \\d <table>          show a table's columns\n"
               "  \\timing             toggle query timing on/off\n"
               "  \\help               show this help\n"
               "  \\quit               exit\n"
               "statements may span lines and end with a semicolon, e.g.\n"
               "  SELECT city, temperature FROM weather\n"
               "    WHERE city = \"London\" ORDER BY temperature DESC;\n"
               "a SELECT with no WHERE returns rows directly (a full scan).\n";
}

// The path of the persistent history file: $OPAQUEDB_HISTORY, else
// $HOME/.opaquedb_history, else empty (history stays in-memory only).
std::string HistoryPath() {
  if (const char *h = std::getenv("OPAQUEDB_HISTORY"); h != nullptr && *h)
    return h;
  if (const char *home = std::getenv("HOME"); home != nullptr && *home)
    return std::string(home) + "/.opaquedb_history";
  return "";
}

// Prints a table's schema as a small table (column, type, encoding).
void PrintDescribe(client::QueryClient &client, const std::string &database,
                   const std::string &table) {
  absl::StatusOr<core::Schema> schema = client.DescribeTable(database, table);
  if (!schema.ok()) {
    std::cout << "error: " << schema.status().message() << "\n";
    return;
  }
  std::vector<std::vector<std::string>> rows;
  for (const core::Column &col : schema->columns()) {
    rows.push_back(
        {col.name, core::ToString(col.type), core::ToString(col.encoding)});
  }
  std::cout << RenderTable({"column", "type", "encoding"}, rows);
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
  repl->callback([this, &globals, &exit_code]() {
    absl::StatusOr<config::Config> config = LoadConfig(globals);
    if (!config.ok()) {
      spdlog::error("repl: {}", config.status().message());
      exit_code = 1;
      return;
    }

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
    bool timing = false;
    // Schemas fetched from the node, cached by "database.table", so rows decode
    // and project without a schema file. Also feeds tab completion.
    std::map<std::string, core::Schema> schema_cache;

    // replxx gives line editing, history (up/down recall), and tab completion.
    replxx::Replxx rx;
    rx.install_window_change_handler();
    rx.set_max_history_size(1000);
    const std::string history_path = HistoryPath();
    if (!history_path.empty())
      rx.history_load(history_path);

    // Completion offers SQL keywords, meta-commands, and the table and column
    // names already learned (queried once, then cached) for the current
    // database.
    rx.set_completion_callback([&](const std::string &context,
                                   int &len) -> replxx::Replxx::completions_t {
      std::vector<std::string> candidates = {
          "SELECT",   "DISTINCT", "FROM",   "WHERE",    "AND",   "OR",
          "ORDER BY", "LIMIT",    "OFFSET", "COUNT(*)", "\\use", "\\tables",
          "\\d",      "\\timing", "\\help", "\\quit"};
      const std::string prefix = database + ".";
      for (const auto &[key, schema] : schema_cache) {
        if (key.compare(0, prefix.size(), prefix) != 0)
          continue;
        candidates.push_back(key.substr(prefix.size())); // table name
        for (const core::Column &col : schema.columns())
          candidates.push_back(col.name);
      }
      const std::size_t ws = context.find_last_of(" \t");
      const std::string word =
          context.substr(ws == std::string::npos ? 0 : ws + 1);
      len = static_cast<int>(word.size());
      replxx::Replxx::completions_t out;
      for (const std::string &cand : candidates)
        if (cand.size() >= word.size() &&
            strncasecmp(cand.c_str(), word.c_str(), word.size()) == 0)
          out.emplace_back(cand);
      return out;
    });
    std::cout << "OpaqueDB shell. \\help for commands, \\quit to exit.\n";

    std::string pending; // accumulated lines of a multi-line statement
    while (true) {
      const std::string prompt =
          pending.empty() ? "opaquedb(" + database + ")> " : "       ...> ";
      const char *cline = rx.input(prompt);
      if (cline == nullptr)
        break; // EOF (Ctrl-D) or interrupt
      const std::string line(cline);

      // A meta-command is a single line beginning with '\', only at the start
      // of a statement.
      if (pending.empty()) {
        const std::string_view trimmed = absl::StripAsciiWhitespace(line);
        if (trimmed.empty())
          continue;
        if (trimmed.front() == '\\') {
          rx.history_add(line);
          const std::pair<std::string_view, std::string_view> parts =
              absl::StrSplit(trimmed, absl::MaxSplits(' ', 1));
          const std::string_view cmd = parts.first;
          const std::string_view arg = absl::StripAsciiWhitespace(parts.second);
          if (cmd == "\\quit" || cmd == "\\q") {
            break;
          } else if (cmd == "\\help" || cmd == "\\h") {
            PrintHelp();
          } else if (cmd == "\\timing") {
            timing = !timing;
            std::cout << "timing is " << (timing ? "on" : "off") << "\n";
          } else if (cmd == "\\d") {
            if (arg.empty())
              std::cout << "usage: \\d <table>\n";
            else
              PrintDescribe(**client, database, std::string(arg));
          } else if (cmd == "\\use") {
            if (arg.empty()) {
              std::cout << "usage: \\use <database>\n";
            } else {
              // Validate against the local catalog so switching to a database
              // with no tables is reported rather than silently accepted.
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
          } else if (cmd == "\\tables") {
            storage::Catalog catalog(config->DatabasesDir());
            absl::StatusOr<std::vector<core::TableId>> ids =
                catalog.ListTables();
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
            std::cout << "unknown command '" << cmd
                      << "'; \\help for the list\n";
          }
          continue;
        }
      }

      // Accumulate into the pending statement. A blank line forces execution of
      // what is buffered; otherwise a statement runs once it ends with ';'.
      const bool blank = absl::StripAsciiWhitespace(line).empty();
      if (!pending.empty())
        pending += "\n";
      pending += line;
      const std::string_view ptrim = absl::StripAsciiWhitespace(pending);
      if (ptrim.empty()) {
        pending.clear();
        continue;
      }
      if (ptrim.back() != ';' && !blank)
        continue; // statement is not finished yet

      const std::string statement(ptrim);
      rx.history_add(pending);
      pending.clear();

      const auto start = std::chrono::steady_clock::now();
      if (absl::Status s = RunSelect(**client, client_id_, statement,
                                     /*value=*/0, /*backend=*/"", database,
                                     std::cout, &schema_cache);
          !s.ok()) {
        std::cout << "error: " << s.message() << "\n";
      } else if (timing) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        std::cout << "Time: " << ms << " ms\n";
      }
    }

    if (!history_path.empty())
      rx.history_save(history_path);
  });
}

} // namespace opaquedb::cli

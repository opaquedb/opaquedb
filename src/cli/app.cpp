#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "CLI/CLI.hpp"
#include "absl/strings/str_split.h"
#include "commands/admin_commands.h"
#include "commands/config_command.h"
#include "commands/data_commands.h"
#include "commands/insert_command.h"
#include "commands/query_command.h"
#include "commands/repl_command.h"
#include "commands/run_command.h"
#include "commands/token_command.h"
#include "opaquedb/cli/command.h"
#include "opaquedb/core/version.h"

namespace opaquedb::cli {

absl::StatusOr<config::LoadOptions>
GlobalOptions::ToLoadOptions(bool file_optional) const {
  config::LoadOptions opts;
  opts.env = config::SystemEnv();
  opts.config_path = config::ResolveConfigPath(config_path, opts.env);
  opts.file_is_optional = file_optional;
  for (const std::string &kv : set_overrides) {
    std::pair<std::string, std::string> parts =
        absl::StrSplit(kv, absl::MaxSplits('=', 1));
    if (parts.second.empty() && kv.find('=') == std::string::npos) {
      return absl::InvalidArgumentError("--set expects key=value, got '" + kv +
                                        "'");
    }
    opts.flag_overrides[parts.first] = parts.second;
  }
  return opts;
}

int Run(int argc, char **argv) {
  CLI::App app{"OpaqueDB: run SQL over encrypted data without revealing the "
               "query."};
  app.set_version_flag("--version", std::string{core::version()});
  app.require_subcommand(0);

  GlobalOptions globals;
  app.add_option(
      "-c,--config", globals.config_path,
      "Config file path (overrides OPAQUEDB_CONFIG and the default)");
  app.add_option("--set", globals.set_overrides,
                 "Override a config key: --set section.key=value (repeatable)");

  int exit_code = 0;

  // Register commands. New subcommands are added here and nowhere else.
  std::vector<std::unique_ptr<Command>> commands;
  commands.push_back(std::make_unique<ConfigCommand>());
  commands.push_back(std::make_unique<LoadCommand>());
  commands.push_back(std::make_unique<InsertCommand>());
  commands.push_back(std::make_unique<RunCommand>());
  commands.push_back(std::make_unique<QueryCommand>());
  commands.push_back(std::make_unique<ReplCommand>());
  commands.push_back(std::make_unique<StatusCommand>());
  commands.push_back(std::make_unique<SchemaCommand>());
  commands.push_back(std::make_unique<EpochCommand>());
  commands.push_back(std::make_unique<TablesCommand>());
  commands.push_back(std::make_unique<TokenCommand>());
  for (auto &command : commands) {
    command->Register(app, globals, exit_code);
  }

  // Let the global options (--config, --set) appear after a subcommand by
  // falling unmatched options through to the parent. Applied to every
  // subcommand, including nested ones.
  std::function<void(CLI::App *)> enable_fallthrough = [&](CLI::App *node) {
    for (CLI::App *sub : node->get_subcommands({})) {
      sub->fallthrough();
      enable_fallthrough(sub);
    }
  };
  enable_fallthrough(&app);

  CLI11_PARSE(app, argc, argv);
  return exit_code;
}

} // namespace opaquedb::cli

#ifndef OPAQUEDB_CLI_COMMAND_H_
#define OPAQUEDB_CLI_COMMAND_H_

#include <string>
#include <vector>

#include "CLI/CLI.hpp"
#include "opaquedb/config/loader.h"

// CLI command objects. Each subcommand is a Command with a common interface, so
// new subcommands extend the binary without touching main. main only wires
// commands together through Run.

namespace opaquedb::cli {

// Options shared by every subcommand: the config path and ad hoc overrides.
// These map onto the loader's layers so the CLI is a thin front to the one
// config resolver. Nothing here re-reads the file or the environment.
struct GlobalOptions {
  // From --config / -c. Empty means fall back to OPAQUEDB_CONFIG then the
  // default path; ResolveConfigPath does that.
  std::string config_path;

  // From repeated --set key=value. These become the flag layer, which overrides
  // the file and the environment. A generic setter keeps every key reachable
  // from the command line without duplicating the schema as one flag per key.
  std::vector<std::string> set_overrides;

  // Builds loader options from the globals. file_optional controls whether a
  // missing config file is an error; `config print` tolerates a missing file,
  // `run` does not.
  absl::StatusOr<config::LoadOptions> ToLoadOptions(bool file_optional) const;
};

// A subcommand. Register adds the subcommand and its options to the parent app
// and installs a callback that runs it. The callback stores its exit code in
// the shared int so Run can return it.
class Command {
public:
  virtual ~Command() = default;
  virtual void Register(CLI::App &parent, const GlobalOptions &globals,
                        int &exit_code) = 0;
};

// Builds the app, registers every command, parses argv, and returns the process
// exit code. This is the only entry the binary needs; main calls it and nothing
// else.
int Run(int argc, char **argv);

} // namespace opaquedb::cli

#endif // OPAQUEDB_CLI_COMMAND_H_

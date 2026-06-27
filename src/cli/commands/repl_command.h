#ifndef OPAQUEDB_CLI_REPL_COMMAND_H_
#define OPAQUEDB_CLI_REPL_COMMAND_H_

#include <string>

#include "opaquedb/cli/command.h"

namespace opaquedb::cli {

// `repl`: an interactive shell over the wire query client. It connects to a
// node, registers keys once, then runs each typed SELECT as a private query.
// Meta-commands start with a backslash: \use picks the database, \tables lists
// what is on disk locally, \help, \quit. The table schema is fetched from the
// node, so rows decode without the user supplying it.
// Inline literals make it natural to type, e.g.
//   SELECT temperature FROM weather WHERE city = "London"
class ReplCommand : public Command {
public:
  void Register(CLI::App &parent, const GlobalOptions &globals,
                int &exit_code) override;

private:
  std::string target_;
  std::string client_id_ = "repl";
  std::string token_;
  std::string database_ = "default";
};

} // namespace opaquedb::cli

#endif // OPAQUEDB_CLI_REPL_COMMAND_H_

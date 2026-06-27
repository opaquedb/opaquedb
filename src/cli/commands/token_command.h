#ifndef OPAQUEDB_CLI_TOKEN_COMMAND_H_
#define OPAQUEDB_CLI_TOKEN_COMMAND_H_

#include <cstdint>
#include <string>

#include "opaquedb/cli/command.h"

namespace opaquedb::cli {

// `token mint --id NAME [--role query|admin] [--bytes N]` prints a
// high-entropy bearer token and the ready-to-paste token-file line for it. It
// helps an operator provision token auth without hand-rolling entropy. The
// token is generated from the system CSPRNG; the command never reads or writes
// the live token file (the operator places the line where they keep secrets).
class TokenCommand : public Command {
public:
  void Register(CLI::App &parent, const GlobalOptions &globals,
                int &exit_code) override;

private:
  std::string id_;
  std::string role_ = "query";
  std::uint32_t bytes_ = 32;
};

} // namespace opaquedb::cli

#endif // OPAQUEDB_CLI_TOKEN_COMMAND_H_

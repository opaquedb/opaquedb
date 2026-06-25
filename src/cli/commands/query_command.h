#ifndef OPAQUEDB_CLI_QUERY_COMMAND_H_
#define OPAQUEDB_CLI_QUERY_COMMAND_H_

#include <cstdint>
#include <string>

#include "opaquedb/cli/command.h"

namespace opaquedb::cli {

// `query`: the dev-only query client. It generates a keypair, encrypts the
// WHERE value, calls Execute, decrypts, and prints the rows. It exists only for
// testing; gRPC is the real client contract.
class QueryCommand : public Command {
public:
  void Register(CLI::App &parent, const GlobalOptions &globals,
                int &exit_code) override;

private:
  std::string sql_;
  std::uint64_t value_ = 0;
  std::string database_ = "default";
  std::string target_;
  std::string client_id_ = "dev";
  std::string backend_;
  std::string token_;
  std::string schema_;
};

} // namespace opaquedb::cli

#endif // OPAQUEDB_CLI_QUERY_COMMAND_H_

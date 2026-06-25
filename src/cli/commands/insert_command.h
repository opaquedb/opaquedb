#ifndef OPAQUEDB_CLI_INSERT_COMMAND_H_
#define OPAQUEDB_CLI_INSERT_COMMAND_H_

#include <string>
#include <vector>

#include "opaquedb/cli/command.h"

namespace opaquedb::cli {

// `insert`: append one row to an existing table over the wire. The values are
// given in the table's declared (CREATE TABLE) column order, including the
// match key. The row is plaintext today; a future encrypted-payload insert
// sends client-encrypted values over the same path.
class InsertCommand : public Command {
public:
  void Register(CLI::App &parent, const GlobalOptions &globals,
                int &exit_code) override;

private:
  std::string table_;
  std::string database_ = "default";
  std::string target_;
  std::string token_;
  std::vector<std::string> values_;
};

} // namespace opaquedb::cli

#endif // OPAQUEDB_CLI_INSERT_COMMAND_H_

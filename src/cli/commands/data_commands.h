#ifndef OPAQUEDB_CLI_DATA_COMMANDS_H_
#define OPAQUEDB_CLI_DATA_COMMANDS_H_

#include <string>

#include "opaquedb/cli/command.h"

namespace opaquedb::cli {

// `load`: read a CREATE TABLE schema and a CSV, build an immutable epoch, and
// publish it through the AdminService. The schema declares the columns, their
// types, and the match key. The CSV's header names the columns.
class LoadCommand : public Command {
public:
  void Register(CLI::App &parent, const GlobalOptions &globals,
                int &exit_code) override;

private:
  std::string schema_;
  std::string csv_;
  // The database that holds this table; the table name comes from the schema.
  std::string database_ = "default";
  // Optional sharding: keep only rows this node owns under the consistent-hash
  // map over the given node list. Each node loads its disjoint shard.
  std::string shard_id_;
  std::string shard_nodes_;
};

} // namespace opaquedb::cli

#endif // OPAQUEDB_CLI_DATA_COMMANDS_H_

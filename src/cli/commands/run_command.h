#ifndef OPAQUEDB_CLI_RUN_COMMAND_H_
#define OPAQUEDB_CLI_RUN_COMMAND_H_

#include "opaquedb/cli/command.h"

namespace opaquedb::cli {

// `run`: start a node and serve the query API. Handles SIGINT and SIGTERM with
// a graceful shutdown that drains in-flight requests. Cluster self-organization
// (etcd lease, election, shard map) is layered on in the cluster step.
class RunCommand : public Command {
public:
  void Register(CLI::App &parent, const GlobalOptions &globals,
                int &exit_code) override;
};

} // namespace opaquedb::cli

#endif // OPAQUEDB_CLI_RUN_COMMAND_H_

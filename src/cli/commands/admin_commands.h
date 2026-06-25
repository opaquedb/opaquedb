#ifndef OPAQUEDB_CLI_ADMIN_COMMANDS_H_
#define OPAQUEDB_CLI_ADMIN_COMMANDS_H_

#include <cstdint>
#include <string>

#include "opaquedb/cli/command.h"

namespace opaquedb::cli {

// Management subcommands. They are thin clients of the AdminService facade,
// which they build in process over the configured data directory, so the
// management logic lives once in AdminService. A running node exposes the same
// facade over gRPC for remote and web-UI clients.

// `status`: node info plus one table's epoch version and row count.
class StatusCommand : public Command {
public:
  void Register(CLI::App &parent, const GlobalOptions &globals,
                int &exit_code) override;

private:
  std::string database_ = "default";
  std::string table_;
};

// `schema inspect`: show one table's schema at its current epoch.
class SchemaCommand : public Command {
public:
  void Register(CLI::App &parent, const GlobalOptions &globals,
                int &exit_code) override;

private:
  std::string database_ = "default";
  std::string table_;
};

// `epoch list|rollback`: inspect and roll back a table's published epochs.
class EpochCommand : public Command {
public:
  void Register(CLI::App &parent, const GlobalOptions &globals,
                int &exit_code) override;

private:
  std::string database_ = "default";
  std::string table_;
  std::uint64_t rollback_version_ = 0;
};

// `tables`: list every database and table present on disk.
class TablesCommand : public Command {
public:
  void Register(CLI::App &parent, const GlobalOptions &globals,
                int &exit_code) override;
};

} // namespace opaquedb::cli

#endif // OPAQUEDB_CLI_ADMIN_COMMANDS_H_

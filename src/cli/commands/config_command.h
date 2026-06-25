#ifndef OPAQUEDB_CLI_CONFIG_COMMAND_H_
#define OPAQUEDB_CLI_CONFIG_COMMAND_H_

#include "opaquedb/cli/command.h"

namespace opaquedb::cli {

// `config init|validate|print`. init writes the commented default file,
// validate checks a file, print shows the effective merged config. All three
// are thin fronts to the config loader.
class ConfigCommand : public Command {
public:
  void Register(CLI::App &parent, const GlobalOptions &globals,
                int &exit_code) override;

private:
  // Storage for parsed options, owned for the lifetime of the app.
  std::string init_output_;
  bool init_force_ = false;
  std::string validate_path_;
};

} // namespace opaquedb::cli

#endif // OPAQUEDB_CLI_CONFIG_COMMAND_H_

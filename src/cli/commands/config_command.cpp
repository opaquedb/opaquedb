#include "config_command.h"

#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "opaquedb/config/loader.h"

namespace opaquedb::cli {
namespace {

// Writes text to a file, or to stdout when path is "-". Refuses to overwrite an
// existing file unless force is set.
absl::Status WriteFileOrStdout(const std::string &path, std::string_view text,
                               bool force) {
  if (path == "-") {
    std::cout << text;
    return absl::OkStatus();
  }
  if (!force) {
    std::error_code ec;
    if (std::ifstream(path).good()) {
      return absl::AlreadyExistsError(
          path + " already exists; pass --force to overwrite");
    }
    (void)ec;
  }
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return absl::UnavailableError("cannot open " + path + " for writing");
  }
  out << text;
  if (!out) {
    return absl::UnavailableError("failed while writing " + path);
  }
  return absl::OkStatus();
}

} // namespace

void ConfigCommand::Register(CLI::App &parent, const GlobalOptions &globals,
                             int &exit_code) {
  CLI::App *config_app =
      parent.add_subcommand("config", "Manage configuration files");
  config_app->require_subcommand(1);

  // config init
  CLI::App *init =
      config_app->add_subcommand("init", "Write a commented default file");
  init->add_option("-o,--output", init_output_,
                   "Destination file, or - for stdout")
      ->default_val("-");
  init->add_flag("--force", init_force_, "Overwrite an existing file");
  init->callback([this, &exit_code]() {
    absl::Status s = WriteFileOrStdout(
        init_output_, config::DefaultConfigToml(), init_force_);
    if (!s.ok()) {
      std::cerr << "config init: " << s.message() << "\n";
      exit_code = 1;
    }
  });

  // config validate
  CLI::App *validate =
      config_app->add_subcommand("validate", "Check a config file for errors");
  validate->add_option("file", validate_path_,
                       "File to validate (default: resolved --config path)");
  validate->callback([this, &globals, &exit_code]() {
    std::string path = validate_path_.empty()
                           ? config::ResolveConfigPath(globals.config_path,
                                                       config::SystemEnv())
                           : validate_path_;
    absl::StatusOr<config::Config> cfg = config::LoadFileOnly(path);
    if (!cfg.ok()) {
      std::cerr << "invalid: " << path << ": " << cfg.status().message()
                << "\n";
      exit_code = 1;
      return;
    }
    std::cout << "ok: " << path << "\n";
  });

  // config print
  CLI::App *print = config_app->add_subcommand(
      "print", "Show the effective merged config as TOML");
  print->callback([&globals, &exit_code]() {
    // The merged view tolerates a missing file so it works before one is
    // written; it still shows defaults plus environment and --set overrides.
    absl::StatusOr<config::LoadOptions> opts =
        globals.ToLoadOptions(/*file_optional=*/true);
    if (!opts.ok()) {
      std::cerr << "config print: " << opts.status().message() << "\n";
      exit_code = 1;
      return;
    }
    absl::StatusOr<config::Config> cfg = config::Load(*opts);
    if (!cfg.ok()) {
      std::cerr << "config print: " << cfg.status().message() << "\n";
      exit_code = 1;
      return;
    }
    std::cout << config::ToToml(*cfg);
  });
}

} // namespace opaquedb::cli

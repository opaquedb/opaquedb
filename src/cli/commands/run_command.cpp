#include "run_command.h"

#include <atomic>
#include <csignal>
#include <thread>

#include "../util.h"
#include "opaquedb/server/node_server.h"
#include "spdlog/spdlog.h"

namespace opaquedb::cli {
namespace {

// The running server, so the signal handler can ask it to shut down. A signal
// handler may touch only async-signal-safe state, so it sets a flag and the
// main thread does the work (shutdown, or an auth reload on SIGHUP).
std::atomic<bool> *g_stop_flag = nullptr;
std::atomic<bool> *g_reload_flag = nullptr;

void OnSignal(int signal) {
  if (signal == SIGHUP) {
    if (g_reload_flag != nullptr)
      g_reload_flag->store(true);
    return;
  }
  if (g_stop_flag != nullptr)
    g_stop_flag->store(true);
}

} // namespace

void RunCommand::Register(CLI::App &parent, const GlobalOptions &globals,
                          int &exit_code) {
  CLI::App *run =
      parent.add_subcommand("run", "Start a node and serve queries");
  run->callback([&globals, &exit_code]() {
    absl::StatusOr<config::Config> config = LoadConfig(globals);
    if (!config.ok()) {
      spdlog::error("run: {}", config.status().message());
      exit_code = 1;
      return;
    }

    absl::StatusOr<std::unique_ptr<server::NodeServer>> node =
        server::NodeServer::Create(*config);
    if (!node.ok()) {
      spdlog::error("run: {}", node.status().message());
      exit_code = 1;
      return;
    }
    if (absl::Status s = (*node)->Start(); !s.ok()) {
      spdlog::error("run: {}", s.message());
      exit_code = 1;
      return;
    }
    spdlog::info("listening on {}", (*node)->listen_address());
    // Echo the security-relevant configuration so an operator can confirm the
    // node came up the way they intended without re-deriving it from the file.
    const char *auth_mode =
        config->auth.mode == config::AuthMode::kToken  ? "token"
        : config->auth.mode == config::AuthMode::kMtls ? "mtls"
                                                       : "none";
    spdlog::info("startup: auth={} cluster={} data_dir={}", auth_mode,
                 config->cluster.enabled ? "enabled" : "disabled",
                 config->node.data_dir);

    std::atomic<bool> stop{false};
    std::atomic<bool> reload{false};
    g_stop_flag = &stop;
    g_reload_flag = &reload;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    // SIGHUP reloads the token file so credentials rotate without a restart.
    std::signal(SIGHUP, OnSignal);

    // Wait for a stop signal, reloading auth whenever SIGHUP arrives.
    while (!stop.load()) {
      if (reload.exchange(false)) {
        if (absl::Status s = (*node)->ReloadAuth(); !s.ok())
          spdlog::error("run: auth reload failed: {}", s.message());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    spdlog::info("shutting down");
    (*node)->Shutdown();
    g_stop_flag = nullptr;
    g_reload_flag = nullptr;
  });
}

} // namespace opaquedb::cli

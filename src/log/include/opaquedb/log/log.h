#ifndef OPAQUEDB_LOG_LOG_H_
#define OPAQUEDB_LOG_LOG_H_

#include "absl/status/status.h"
#include "opaquedb/config/config.h"
#include "spdlog/spdlog.h"

// The one place logging is configured. Every component writes through spdlog
// after Init has run, so all messages share one destination, one level, and one
// format. The destination (a file or standard output), the level, and the
// format (json or text) come from config.logging, the single source of truth.
//
// Use the spdlog free functions for messages: spdlog::info, spdlog::warn,
// spdlog::error, and so on. Include this header and link opaquedb_log; it pulls
// spdlog in for you.

namespace opaquedb::log {

// Configures the global logger from the logging config. Safe to call more than
// once; the last call wins. Returns an error only if a log file is configured
// but cannot be opened.
absl::Status Init(const config::LoggingConfig &cfg);

} // namespace opaquedb::log

#endif // OPAQUEDB_LOG_LOG_H_

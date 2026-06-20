#include "opaquedb/log/log.h"

#include <exception>
#include <memory>
#include <vector>

#include "absl/strings/str_cat.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/stdout_sinks.h"

namespace opaquedb::log {
namespace {

spdlog::level::level_enum LevelFromString(const std::string &level) {
  // The loader validates the spelling, so an unknown value here only happens if
  // the two lists drift. Fall back to info rather than refuse to log.
  spdlog::level::level_enum parsed = spdlog::level::from_str(level);
  if (parsed == spdlog::level::off && level != "off")
    return spdlog::level::info;
  return parsed;
}

// Text is meant for humans at a terminal, so it carries a timestamp and a
// colored level. Json is one object per line for log shippers; it keeps the
// message on the "msg" field and adds no color codes.
const char *PatternFor(config::LogFormat format) {
  switch (format) {
  case config::LogFormat::kText:
    return "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v";
  case config::LogFormat::kJson:
    return R"({"time":"%Y-%m-%dT%H:%M:%S.%e%z","level":"%l","msg":"%v"})";
  }
  return "%v";
}

} // namespace

absl::Status Init(const config::LoggingConfig &cfg) {
  try {
    spdlog::sink_ptr sink;
    if (!cfg.file.empty()) {
      sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
          cfg.file,
          /*truncate=*/false);
    } else if (cfg.format == config::LogFormat::kText) {
      sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    } else {
      // Json output stays free of terminal color codes.
      sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
    }

    auto logger = std::make_shared<spdlog::logger>("opaquedb", std::move(sink));
    logger->set_pattern(PatternFor(cfg.format));
    logger->set_level(LevelFromString(cfg.level));
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(std::move(logger));
    return absl::OkStatus();
  } catch (const std::exception &e) {
    return absl::UnavailableError(
        absl::StrCat("cannot open log file '", cfg.file, "': ", e.what()));
  }
}

} // namespace opaquedb::log

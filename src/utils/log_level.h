/**
 * @file log_level.h
 * @brief Mapping of configuration log-level names to spdlog levels
 */

#pragma once

#include <spdlog/spdlog.h>

#include <optional>
#include <string_view>

namespace nvecd::utils {

/**
 * @brief Parse a logging.level name.
 *
 * The accepted set is exactly trace, debug, info, warn and error (lowercase),
 * the values the configuration schema allows.
 *
 * @param name Level name from configuration or a runtime variable.
 * @return The spdlog level, or std::nullopt for any other text.
 */
inline std::optional<spdlog::level::level_enum> ParseLogLevel(std::string_view name) {
  if (name == "trace") {
    return spdlog::level::trace;
  }
  if (name == "debug") {
    return spdlog::level::debug;
  }
  if (name == "info") {
    return spdlog::level::info;
  }
  if (name == "warn") {
    return spdlog::level::warn;
  }
  if (name == "error") {
    return spdlog::level::err;
  }
  return std::nullopt;
}

}  // namespace nvecd::utils

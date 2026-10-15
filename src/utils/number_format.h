/**
 * @file number_format.h
 * @brief Locale-independent number formatting
 */

#pragma once

#include <charconv>
#include <string>
#include <system_error>

namespace nvecd::utils {

/**
 * @brief Format a double in its shortest round-trippable decimal form.
 *
 * Non-finite values come out as "nan", "inf" or "-inf".
 */
inline std::string FormatShortestDouble(double value) {
  char buffer[64];
  const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
  if (ec == std::errc{}) {
    return {buffer, end};
  }
  // Only reached on platforms whose floating-point to_chars reports an error.
  return std::to_string(value);
}

}  // namespace nvecd::utils

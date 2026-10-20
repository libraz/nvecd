/**
 * @file wire_limits.h
 * @brief Limits and byte rules of the line protocol shared by clients and the server
 */

#pragma once

#include <cstddef>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace nvecd::utils {

/// Longest AUTH password any surface accepts or sends.
constexpr size_t kMaxPasswordBytes = 4096;

/**
 * @brief Reject bytes the line protocol cannot carry.
 *
 * The wire protocol is the authority on what a command may contain: the server
 * refuses an embedded NUL and an embedded CR/LF and accepts every other byte
 * (see ParseCommand in src/server/command_parser.cpp). Rejecting more than
 * that - a blanket std::iscntrl test also excludes TAB - makes the shipped
 * clients refuse input the server and a hand-written socket client accept.
 *
 * These three bytes are exactly the ones that would break framing: NUL
 * terminates the command for the parser, CR/LF splits one command into two.
 * Every client surface checks the finished command line with this one
 * function before any byte is sent.
 *
 * @param value Text to check.
 * @param field_name Human-readable name used in the message.
 * @return std::nullopt when the text is carriable, otherwise the reason.
 */
inline std::optional<std::string> ValidateNoFramingBytes(std::string_view value, const char* field_name) {
  for (unsigned char character : value) {
    if (character == '\0' || character == '\r' || character == '\n') {
      std::ostringstream oss;
      oss << "Input for " << field_name << " contains byte 0x" << std::uppercase << std::hex << std::setw(2)
          << std::setfill('0') << static_cast<int>(character) << ", which the line protocol cannot carry";
      return oss.str();
    }
  }
  return std::nullopt;
}

/**
 * @brief Whether @p value can be written as one token of a line-framed reply
 *
 * Identifiers and snapshot paths are echoed unescaped into replies whose
 * clients split on spaces and count newlines, so no byte at or below 0x20
 * (space and every control character) and no DEL may appear. Bytes at or
 * above 0x80 pass so UTF-8 names keep working.
 */
constexpr bool IsSingleToken(std::string_view value) {
  constexpr unsigned char kSpace = 0x20;
  constexpr unsigned char kDelete = 0x7F;
  for (const char raw : value) {
    const auto byte = static_cast<unsigned char>(raw);
    if (byte <= kSpace || byte == kDelete) {
      return false;
    }
  }
  return true;
}

}  // namespace nvecd::utils

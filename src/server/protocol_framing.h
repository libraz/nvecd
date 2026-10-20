/**
 * @file protocol_framing.h
 * @brief Shared request-line parsing and response framing for TCP handlers.
 */

#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace nvecd::server {

struct RequestFrame {
  size_t request_size;
  size_t delimiter_size;
};

/// Find the first LF-delimited request, treating a CR immediately before LF as
/// part of the delimiter. Looking for LF first keeps mixed LF/CRLF pipelines in
/// the same order on the legacy and reactor handlers.
inline std::optional<RequestFrame> FindRequestFrame(std::string_view input) {
  const size_t newline = input.find('\n');
  if (newline == std::string_view::npos) {
    return std::nullopt;
  }
  const bool has_cr = newline > 0 && input[newline - 1] == '\r';
  return RequestFrame{newline - static_cast<size_t>(has_cr), 1 + static_cast<size_t>(has_cr)};
}

/// Parse a RESP bulk header line ("$<n>") into its payload length.
inline std::optional<size_t> BulkPayloadLength(std::string_view line) {
  if (line.size() < 2 || line.front() != '$') {
    return std::nullopt;
  }
  size_t length = 0;
  const char* const end = line.data() + line.size();
  const auto [parsed_end, error] = std::from_chars(line.data() + 1, end, length);
  if (error != std::errc{} || parsed_end != end) {
    return std::nullopt;
  }
  return length;
}

/**
 * @brief Frame handler output as one CRLF-terminated wire reply
 *
 * Handlers mix bare LF and CRLF, so every line terminator becomes CRLF and an
 * unterminated last line gets one; no content line is dropped, including the
 * empty payload line of "$0". An ERROR reply is collapsed onto a single line
 * because clients end it at the first terminator. In a RESP reply ('$' or '*'
 * first) each bulk payload is copied verbatim so it keeps its declared length.
 */
inline std::string NormalizeResponse(std::string_view response) {
  std::string normalized;
  normalized.reserve(response.size() + 2);

  if (response.rfind("ERROR", 0) == 0) {
    bool in_break = false;
    for (const char character : response) {
      if (character == '\r' || character == '\n') {
        in_break = true;
        continue;
      }
      if (in_break) {
        normalized += ' ';
        in_break = false;
      }
      normalized += character;
    }
    normalized += "\r\n";
    return normalized;
  }

  const bool resp = !response.empty() && (response.front() == '$' || response.front() == '*');
  size_t pos = 0;
  while (pos < response.size()) {
    const size_t newline = response.find('\n', pos);
    const size_t line_end = newline == std::string_view::npos ? response.size() : newline;
    std::string_view line = response.substr(pos, line_end - pos);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    for (const char character : line) {
      if (character != '\r') {
        normalized += character;
      }
    }
    normalized += "\r\n";
    pos = newline == std::string_view::npos ? response.size() : newline + 1;

    const auto payload_length = resp ? BulkPayloadLength(line) : std::nullopt;
    if (payload_length) {
      const size_t payload_end = pos + std::min(response.size() - pos, *payload_length);
      normalized.append(response.substr(pos, payload_end - pos));
      normalized += "\r\n";
      pos = payload_end;
      if (response.compare(pos, 2, "\r\n") == 0) {
        pos += 2;
      } else if (pos < response.size() && response[pos] == '\n') {
        ++pos;
      }
    }
  }
  if (normalized.empty()) {
    normalized = "\r\n";
  }
  return normalized;
}

}  // namespace nvecd::server

/**
 * @file protocol_framing.h
 * @brief Shared request-line parsing and response framing for TCP handlers.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

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

/// Normalize handler output to one CRLF-terminated response: handlers mix bare LF
/// endings and clients split replies on CRLF, so trailing CR/LF is replaced, not kept.
inline std::string NormalizeResponse(std::string_view response) {
  size_t end = response.size();
  while (end > 0 && (response[end - 1] == '\r' || response[end - 1] == '\n')) {
    --end;
  }

  std::string normalized;
  normalized.reserve(end + 2);
  for (size_t i = 0; i < end; ++i) {
    if (response[i] == '\r') {
      continue;
    }
    if (response[i] == '\n') {
      normalized += "\r\n";
    } else {
      normalized += response[i];
    }
  }
  normalized += "\r\n";
  return normalized;
}

}  // namespace nvecd::server

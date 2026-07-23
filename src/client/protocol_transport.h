/**
 * @file protocol_transport.h
 * @brief Shared TCP response framing and reliable socket I/O for clients
 */

#pragma once

#include <sys/socket.h>

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace nvecd::client::transport {

struct ResponseFrame {
  bool end_terminated = false;
  bool debug_similarity = false;
};

inline std::string UpperCommand(std::string_view command) {
  while (!command.empty() && std::isspace(static_cast<unsigned char>(command.front())) != 0) {
    command.remove_prefix(1);
  }
  while (!command.empty() && std::isspace(static_cast<unsigned char>(command.back())) != 0) {
    command.remove_suffix(1);
  }
  std::string upper(command);
  for (char& character : upper) {
    character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  }
  return upper;
}

inline bool StartsWithCommand(const std::string& command, std::string_view prefix) {
  return command == prefix || (command.size() > prefix.size() && command.compare(0, prefix.size(), prefix) == 0 &&
                               std::isspace(static_cast<unsigned char>(command[prefix.size()])) != 0);
}

inline ResponseFrame FrameForCommand(std::string_view command, bool debug_mode) {
  const std::string upper = UpperCommand(command);
  const bool similarity = StartsWithCommand(upper, "SIM") || StartsWithCommand(upper, "SIMV");
  ResponseFrame frame;
  frame.end_terminated = upper == "INFO" || StartsWithCommand(upper, "CONFIG") || upper == "CACHE STATS" ||
                         upper == "DUMP INFO" || upper == "DUMP STATUS";
  frame.debug_similarity = debug_mode && similarity;
  return frame;
}

inline std::optional<int64_t> ParseDecimal(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  int64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

inline std::optional<size_t> LineEnd(const std::string& buffer, size_t start) {
  const size_t newline = buffer.find('\n', start);
  return newline == std::string::npos ? std::nullopt : std::optional<size_t>(newline + 1);
}

inline std::string_view LineText(const std::string& buffer, size_t start, size_t end) {
  size_t length = end - start - 1;
  if (length > 0 && buffer[start + length - 1] == '\r') {
    --length;
  }
  return std::string_view(buffer.data() + start, length);
}

inline std::optional<size_t> CompleteValueLength(const std::string& buffer, size_t start, unsigned depth = 0) {
  constexpr unsigned kMaxNestingDepth = 8;
  if (depth > kMaxNestingDepth) {
    return LineEnd(buffer, start);
  }
  const auto line_end = LineEnd(buffer, start);
  if (!line_end) {
    return std::nullopt;
  }
  const std::string_view line = LineText(buffer, start, *line_end);
  if (line.empty()) {
    return *line_end;
  }

  if (line.front() == '$') {
    const auto length = ParseDecimal(line.substr(1));
    if (!length || *length < -1) {
      return *line_end;
    }
    if (*length == -1) {
      return *line_end;
    }
    if (static_cast<uint64_t>(*length) > std::numeric_limits<size_t>::max() - *line_end - 2) {
      return *line_end;
    }
    const size_t payload_end = *line_end + static_cast<size_t>(*length);
    if (payload_end >= buffer.size()) {
      return std::nullopt;
    }
    if (buffer[payload_end] == '\n') {
      return payload_end + 1;
    }
    if (buffer[payload_end] == '\r') {
      if (payload_end + 1 >= buffer.size()) {
        return std::nullopt;
      }
      return buffer[payload_end + 1] == '\n' ? std::optional<size_t>(payload_end + 2)
                                             : std::optional<size_t>(*line_end);
    }
    return *line_end;
  }

  if (line.front() == '*') {
    const auto count = ParseDecimal(line.substr(1));
    if (!count || *count < -1) {
      return *line_end;
    }
    if (*count == -1) {
      return *line_end;
    }
    size_t cursor = *line_end;
    for (int64_t index = 0; index < *count; ++index) {
      const auto item_end = CompleteValueLength(buffer, cursor, depth + 1);
      if (!item_end) {
        return std::nullopt;
      }
      if (*item_end <= cursor) {
        return *line_end;
      }
      cursor = *item_end;
    }
    return cursor;
  }

  return *line_end;
}

inline std::optional<size_t> CompleteResponseLength(const std::string& buffer, ResponseFrame frame = {}) {
  const auto first_line_end = LineEnd(buffer, 0);
  if (!first_line_end) {
    return std::nullopt;
  }
  const std::string_view header = LineText(buffer, 0, *first_line_end);
  const bool error = header.rfind("ERROR", 0) == 0 || header.rfind("-ERR", 0) == 0 || header.rfind("(error)", 0) == 0;

  if (frame.end_terminated && !error) {
    constexpr std::string_view kEndCrLf = "\nEND\r\n";
    constexpr std::string_view kEndLf = "\nEND\n";
    const size_t crlf_pos = buffer.find(kEndCrLf);
    const size_t lf_pos = buffer.find(kEndLf);
    if (crlf_pos != std::string::npos && (lf_pos == std::string::npos || crlf_pos <= lf_pos)) {
      return crlf_pos + kEndCrLf.size();
    }
    if (lf_pos != std::string::npos) {
      return lf_pos + kEndLf.size();
    }
    return std::nullopt;
  }

  constexpr std::string_view kResultsPrefix = "OK RESULTS ";
  if (header.rfind(kResultsPrefix, 0) == 0) {
    const auto count = ParseDecimal(header.substr(kResultsPrefix.size()));
    if (!count || *count < 0 || static_cast<uint64_t>(*count) > std::numeric_limits<size_t>::max() - 1) {
      return *first_line_end;
    }
    const size_t expected_lines = static_cast<size_t>(*count) + 1;
    size_t newline_count = 0;
    for (size_t index = 0; index < buffer.size(); ++index) {
      if (buffer[index] == '\n' && ++newline_count >= expected_lines) {
        size_t cursor = index + 1;
        if (!frame.debug_similarity) {
          return cursor;
        }
        const auto debug_header_end = LineEnd(buffer, cursor);
        if (!debug_header_end) {
          return std::nullopt;
        }
        constexpr std::string_view kDebugPrefix = "# DEBUG ";
        const std::string_view debug_header = LineText(buffer, cursor, *debug_header_end);
        if (debug_header.rfind(kDebugPrefix, 0) != 0) {
          return *debug_header_end;
        }
        const auto field_count = ParseDecimal(debug_header.substr(kDebugPrefix.size()));
        if (!field_count || *field_count < 0) {
          return *debug_header_end;
        }
        cursor = *debug_header_end;
        for (int64_t field = 0; field < *field_count; ++field) {
          const auto field_end = LineEnd(buffer, cursor);
          if (!field_end) {
            return std::nullopt;
          }
          cursor = *field_end;
        }
        return cursor;
      }
    }
    return std::nullopt;
  }

  return CompleteValueLength(buffer, 0);
}

inline bool ConfigureNoSigpipe(int socket_fd) {
#ifdef SO_NOSIGPIPE
  const int enabled = 1;
  return ::setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
#else
  (void)socket_fd;
  return true;
#endif
}

inline bool SendAll(int socket_fd, std::string_view bytes, int* error_out = nullptr) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const ssize_t sent = ::send(socket_fd, bytes.data() + offset, bytes.size() - offset, flags);
    if (sent > 0) {
      offset += static_cast<size_t>(sent);
      continue;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    const int saved_error = sent == 0 ? EPIPE : errno;
    if (error_out != nullptr) {
      *error_out = saved_error;
    }
    return false;
  }
  return true;
}

inline ssize_t ReceiveRetryingEintr(int socket_fd, void* buffer, size_t length, int* error_out = nullptr) {
  while (true) {
    const ssize_t received = ::recv(socket_fd, buffer, length, 0);
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0 && error_out != nullptr) {
      *error_out = errno;
    }
    return received;
  }
}

}  // namespace nvecd::client::transport

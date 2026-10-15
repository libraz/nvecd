/**
 * @file protocol_transport.h
 * @brief Shared TCP response framing and reliable socket I/O for clients
 */

#pragma once

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace nvecd::client::transport {

struct ResponseFrame {
  bool end_terminated = false;
  bool debug_similarity = false;
  bool similarity_search = false;
  bool long_running = false;  ///< Snapshot save/load/verify: the server may take far longer than a query to reply
};

/**
 * @brief Incremental state for scanning one response from a socket.
 *
 * A scanner belongs to one request and is reused after every recv(). Its
 * cursor advances only through newly received bytes, so a large, segmented
 * response is scanned once instead of from its beginning after each packet.
 */
struct ResponseScanner {
  enum class Mode { kHeader, kEndLines, kResultLines, kDebugHeader, kDebugLines, kValue, kDone };

  explicit ResponseScanner(ResponseFrame response_frame = {}) : frame(response_frame) {}

  ResponseFrame frame;
  Mode mode = Mode::kHeader;
  size_t scan_offset = 0;
  size_t line_start = 0;
  size_t lines_remaining = 0;
  size_t completed_length = 0;

  // State used by the RESP-like bulk-string/array fallback.
  size_t value_cursor = 0;
  size_t value_scan_offset = 0;
  size_t pending_bulk_header_end = 0;
  size_t pending_bulk_payload_end = 0;
  bool pending_bulk = false;
  std::vector<size_t> array_items_remaining;
};

struct ParsedSearchResult {
  std::string id;
  float score = 0.0F;
};

/**
 * @brief Strictly parsed SIM/SIMV response, with an optional DEBUG block view.
 *
 * `debug_block` refers to the original response string and remains valid for
 * as long as that string does.
 */
struct ParsedSearchResponse {
  std::vector<ParsedSearchResult> results;
  std::string_view debug_block;
};

// Upper bound on the bytes a client buffers for one response. A peer that never
// completes a frame would otherwise grow the buffer without limit, so both the
// library and the CLI stop at the same point rather than at two limits that can
// drift apart.
constexpr size_t kMaxBufferedResponseBytes = 64U * 1024U * 1024U;

// Receive deadline for snapshot save/load/verify, which can legitimately outlast
// a query by far (ResponseFrame::long_running).
constexpr uint32_t kAdminTimeoutMs = 3600U * 1000U;

// Longest AUTH password any client surface sends.
constexpr size_t kMaxPasswordBytes = 4096;

constexpr std::string_view kErrorPrefix = "ERROR ";

inline bool EqualsAsciiIgnoreCase(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t index = 0; index < lhs.size(); ++index) {
    unsigned char left = static_cast<unsigned char>(lhs[index]);
    unsigned char right = static_cast<unsigned char>(rhs[index]);
    if (left >= 'a' && left <= 'z') {
      left = static_cast<unsigned char>(left - ('a' - 'A'));
    }
    if (right >= 'a' && right <= 'z') {
      right = static_cast<unsigned char>(right - ('a' - 'A'));
    }
    if (left != right) {
      return false;
    }
  }
  return true;
}

inline std::string_view NextToken(std::string_view& input) {
  while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())) != 0) {
    input.remove_prefix(1);
  }
  size_t length = 0;
  while (length < input.size() && std::isspace(static_cast<unsigned char>(input[length])) == 0) {
    ++length;
  }
  const std::string_view token = input.substr(0, length);
  input.remove_prefix(length);
  return token;
}

inline bool HasResponsePrefixToken(std::string_view response, std::string_view prefix) {
  return response.size() >= prefix.size() && response.substr(0, prefix.size()) == prefix &&
         (response.size() == prefix.size() || std::isspace(static_cast<unsigned char>(response[prefix.size()])) != 0);
}

/**
 * @brief Update per-connection DEBUG state only from the server's exact reply.
 *
 * DEBUG is connection state. The reply is its authority: a generic OK or a
 * successful-looking reply with extra bytes must not change response framing,
 * or the read loop waits for a DEBUG block the server never sends.
 */
inline void UpdateDebugMode(std::string_view response, bool& debug_mode) {
  if (response == "OK DEBUG_ON") {
    debug_mode = true;
  } else if (response == "OK DEBUG_OFF") {
    debug_mode = false;
  }
}

/**
 * @brief Whether a response line is an error reply.
 *
 * The server answers a failure with "ERROR ...", and the Redis-style "-ERR ..."
 * and "(error) ..." spellings are accepted alongside it; clients also
 * synthesize "(error) ..." locally for transport failures they report through
 * the same channel. Response framing, error reporting and the DEBUG mode latch
 * all have to agree on what counts as an error, so they classify from this one
 * predicate rather than each spelling out the prefix list.
 */
inline bool IsErrorResponse(std::string_view response) {
  return response.rfind("ERROR", 0) == 0 || response.rfind("-ERR", 0) == 0 || response.rfind("(error)", 0) == 0;
}

/**
 * @brief The human-readable body of an error reply.
 *
 * Strips the "ERROR " prefix; a reply without it (a bare "ERROR", "-ERR ...")
 * is returned whole.
 */
inline std::string_view ErrorBody(std::string_view response) {
  if (response.substr(0, kErrorPrefix.size()) == kErrorPrefix) {
    response.remove_prefix(kErrorPrefix.size());
  }
  return response;
}

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
 * @brief Classify a command's reply framing.
 *
 * This is the one table of multi-line replies: the verb tokens decide, so
 * "DUMP INFO <file>" frames like "DUMP INFO" and extra whitespace between
 * tokens does not change the answer.
 */
inline ResponseFrame FrameForCommand(std::string_view command, bool debug_mode) {
  const std::string_view verb = NextToken(command);
  const std::string_view subcommand = NextToken(command);

  ResponseFrame frame;
  const bool info = EqualsAsciiIgnoreCase(verb, "INFO");
  const bool config = EqualsAsciiIgnoreCase(verb, "CONFIG");
  const bool cache = EqualsAsciiIgnoreCase(verb, "CACHE");
  const bool dump = EqualsAsciiIgnoreCase(verb, "DUMP");
  frame.similarity_search = EqualsAsciiIgnoreCase(verb, "SIM") || EqualsAsciiIgnoreCase(verb, "SIMV");
  frame.end_terminated =
      (info && subcommand.empty()) || config || (cache && EqualsAsciiIgnoreCase(subcommand, "STATS")) ||
      (dump && (EqualsAsciiIgnoreCase(subcommand, "INFO") || EqualsAsciiIgnoreCase(subcommand, "STATUS")));
  frame.debug_similarity = debug_mode && frame.similarity_search;
  frame.long_running =
      dump && (EqualsAsciiIgnoreCase(subcommand, "SAVE") || EqualsAsciiIgnoreCase(subcommand, "LOAD") ||
               EqualsAsciiIgnoreCase(subcommand, "VERIFY"));
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

inline std::string_view LineText(const std::string& buffer, size_t start, size_t end) {
  size_t length = end - start - 1;
  if (length > 0 && buffer[start + length - 1] == '\r') {
    --length;
  }
  return std::string_view(buffer.data() + start, length);
}

inline bool ReadResponseLine(std::string_view response, size_t start, std::string_view& line, size_t& next) {
  if (start >= response.size()) {
    return false;
  }
  const size_t newline = response.find('\n', start);
  if (newline == std::string_view::npos) {
    line = response.substr(start);
    next = response.size();
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    return true;
  }
  size_t length = newline - start;
  if (length > 0 && response[start + length - 1] == '\r') {
    --length;
  }
  line = response.substr(start, length);
  next = newline + 1;
  return true;
}

/**
 * @brief Parse a complete SIM/SIMV response, rejecting partial or malformed rows.
 */
inline std::optional<ParsedSearchResponse> ParseSearchResponse(std::string_view response) {
  std::string_view line;
  size_t cursor = 0;
  size_t next = 0;
  if (!ReadResponseLine(response, cursor, line, next)) {
    return std::nullopt;
  }

  std::string_view header = line;
  const std::string_view status = NextToken(header);
  const std::string_view results_token = NextToken(header);
  const std::string_view count_token = NextToken(header);
  if (status != "OK" || results_token != "RESULTS" || count_token.empty() || !NextToken(header).empty()) {
    return std::nullopt;
  }
  size_t count = 0;
  const auto [count_end, count_error] =
      std::from_chars(count_token.data(), count_token.data() + count_token.size(), count);
  if (count_error != std::errc{} || count_end != count_token.data() + count_token.size()) {
    return std::nullopt;
  }

  ParsedSearchResponse parsed;
  parsed.results.reserve(std::min<size_t>(count, 256));
  cursor = next;
  for (size_t index = 0; index < count; ++index) {
    if (!ReadResponseLine(response, cursor, line, next)) {
      return std::nullopt;
    }
    std::string_view row = line;
    const std::string_view id = NextToken(row);
    const std::string_view score_token = NextToken(row);
    if (id.empty() || score_token.empty() || !NextToken(row).empty()) {
      return std::nullopt;
    }
    float score = 0.0F;
    const auto [score_end, score_error] =
        std::from_chars(score_token.data(), score_token.data() + score_token.size(), score, std::chars_format::general);
    if (score_error != std::errc{} || score_end != score_token.data() + score_token.size() || !std::isfinite(score)) {
      return std::nullopt;
    }
    parsed.results.push_back({std::string(id), score});
    cursor = next;
  }

  if (cursor == response.size()) {
    return parsed;
  }

  const size_t debug_start = cursor;
  if (!ReadResponseLine(response, cursor, line, next)) {
    return std::nullopt;
  }
  std::string_view debug_header = line;
  if (NextToken(debug_header) != "#" || NextToken(debug_header) != "DEBUG") {
    return std::nullopt;
  }
  const std::string_view field_count_token = NextToken(debug_header);
  if (field_count_token.empty() || !NextToken(debug_header).empty()) {
    return std::nullopt;
  }
  size_t field_count = 0;
  const auto [field_count_end, field_count_error] =
      std::from_chars(field_count_token.data(), field_count_token.data() + field_count_token.size(), field_count);
  if (field_count_error != std::errc{} || field_count_end != field_count_token.data() + field_count_token.size()) {
    return std::nullopt;
  }

  cursor = next;
  for (size_t field = 0; field < field_count; ++field) {
    if (!ReadResponseLine(response, cursor, line, next)) {
      return std::nullopt;
    }
    cursor = next;
  }
  if (cursor != response.size()) {
    return std::nullopt;
  }
  parsed.debug_block = response.substr(debug_start);
  return parsed;
}

inline std::optional<size_t> ScanValueLength(const std::string& buffer, ResponseScanner& scanner) {
  constexpr unsigned kMaxNestingDepth = 8;
  const auto completed_value = [&scanner](size_t end) -> bool {
    while (!scanner.array_items_remaining.empty()) {
      size_t& remaining = scanner.array_items_remaining.back();
      --remaining;
      if (remaining > 0) {
        scanner.value_cursor = end;
        scanner.value_scan_offset = end;
        return false;
      }
      scanner.array_items_remaining.pop_back();
    }
    scanner.completed_length = end;
    return true;
  };

  while (true) {
    if (scanner.pending_bulk) {
      const size_t payload_end = scanner.pending_bulk_payload_end;
      if (payload_end >= buffer.size()) {
        return std::nullopt;
      }
      size_t value_end = scanner.pending_bulk_header_end;
      if (buffer[payload_end] == '\n') {
        value_end = payload_end + 1;
      } else if (buffer[payload_end] == '\r') {
        if (payload_end + 1 >= buffer.size()) {
          return std::nullopt;
        }
        if (buffer[payload_end + 1] == '\n') {
          value_end = payload_end + 2;
        }
      }
      scanner.pending_bulk = false;
      scanner.value_cursor = value_end;
      scanner.value_scan_offset = value_end;
      if (completed_value(value_end)) {
        return scanner.completed_length;
      }
      continue;
    }

    const size_t line_start = scanner.value_cursor;
    const size_t search_start = std::max(scanner.value_scan_offset, line_start);
    const size_t newline = buffer.find('\n', search_start);
    if (newline == std::string::npos) {
      scanner.value_scan_offset = buffer.size();
      return std::nullopt;
    }
    const size_t line_end = newline + 1;
    const std::string_view line = LineText(buffer, line_start, line_end);
    scanner.value_scan_offset = line_end;

    if (scanner.array_items_remaining.size() > kMaxNestingDepth) {
      scanner.value_cursor = line_end;
      if (completed_value(line_end)) {
        return scanner.completed_length;
      }
      continue;
    }

    if (line.empty()) {
      scanner.value_cursor = line_end;
      if (completed_value(line_end)) {
        return scanner.completed_length;
      }
      continue;
    }

    if (line.front() == '$') {
      const auto length = ParseDecimal(line.substr(1));
      if (!length || *length < -1 || *length == -1 ||
          static_cast<uint64_t>(*length) > std::numeric_limits<size_t>::max() - line_end - 2) {
        scanner.value_cursor = line_end;
        if (completed_value(line_end)) {
          return scanner.completed_length;
        }
        continue;
      }
      scanner.pending_bulk = true;
      scanner.pending_bulk_header_end = line_end;
      scanner.pending_bulk_payload_end = line_end + static_cast<size_t>(*length);
      continue;
    }

    if (line.front() == '*') {
      const auto count = ParseDecimal(line.substr(1));
      if (!count || *count < -1 || *count == -1 || *count == 0 ||
          static_cast<uint64_t>(*count) > std::numeric_limits<size_t>::max()) {
        scanner.value_cursor = line_end;
        if (completed_value(line_end)) {
          return scanner.completed_length;
        }
        continue;
      }
      scanner.array_items_remaining.push_back(static_cast<size_t>(*count));
      scanner.value_cursor = line_end;
      scanner.value_scan_offset = line_end;
      continue;
    }

    scanner.value_cursor = line_end;
    if (completed_value(line_end)) {
      return scanner.completed_length;
    }
  }
}

inline std::optional<size_t> ScanResponseLength(const std::string& buffer, ResponseScanner& scanner) {
  const auto finish = [&scanner](size_t length) {
    scanner.completed_length = length;
    scanner.mode = ResponseScanner::Mode::kDone;
    return std::optional<size_t>(length);
  };
  const auto next_line = [&buffer, &scanner](std::string_view& line) -> std::optional<size_t> {
    const size_t newline = buffer.find('\n', scanner.scan_offset);
    if (newline == std::string::npos) {
      scanner.scan_offset = buffer.size();
      return std::nullopt;
    }
    const size_t end = newline + 1;
    line = LineText(buffer, scanner.line_start, end);
    scanner.scan_offset = end;
    scanner.line_start = end;
    return end;
  };

  if (scanner.mode == ResponseScanner::Mode::kDone) {
    return scanner.completed_length;
  }

  constexpr std::string_view kResultsPrefix = "OK RESULTS ";
  constexpr std::string_view kDebugPrefix = "# DEBUG ";
  while (true) {
    std::string_view line;
    switch (scanner.mode) {
      case ResponseScanner::Mode::kHeader: {
        const auto line_end = next_line(line);
        if (!line_end) {
          return std::nullopt;
        }
        if (IsErrorResponse(line)) {
          return finish(*line_end);
        }
        if (scanner.frame.end_terminated) {
          scanner.mode = ResponseScanner::Mode::kEndLines;
          continue;
        }
        if (line.rfind(kResultsPrefix, 0) == 0) {
          const auto count = ParseDecimal(line.substr(kResultsPrefix.size()));
          if (!count || *count < 0) {
            return finish(*line_end);
          }
          scanner.lines_remaining = static_cast<size_t>(*count);
          if (scanner.lines_remaining == 0) {
            scanner.mode =
                scanner.frame.debug_similarity ? ResponseScanner::Mode::kDebugHeader : ResponseScanner::Mode::kDone;
            if (scanner.mode == ResponseScanner::Mode::kDone) {
              return finish(*line_end);
            }
          } else {
            scanner.mode = ResponseScanner::Mode::kResultLines;
          }
          continue;
        }
        scanner.mode = ResponseScanner::Mode::kValue;
        scanner.value_cursor = 0;
        scanner.value_scan_offset = 0;
        continue;
      }
      case ResponseScanner::Mode::kEndLines: {
        const auto line_end = next_line(line);
        if (!line_end) {
          return std::nullopt;
        }
        if (line == "END") {
          return finish(*line_end);
        }
        continue;
      }
      case ResponseScanner::Mode::kResultLines: {
        const auto line_end = next_line(line);
        if (!line_end) {
          return std::nullopt;
        }
        --scanner.lines_remaining;
        if (scanner.lines_remaining == 0) {
          if (scanner.frame.debug_similarity) {
            scanner.mode = ResponseScanner::Mode::kDebugHeader;
          } else {
            return finish(*line_end);
          }
        }
        continue;
      }
      case ResponseScanner::Mode::kDebugHeader: {
        const auto line_end = next_line(line);
        if (!line_end) {
          return std::nullopt;
        }
        if (line.rfind(kDebugPrefix, 0) != 0) {
          return finish(*line_end);
        }
        const auto field_count = ParseDecimal(line.substr(kDebugPrefix.size()));
        if (!field_count || *field_count < 0) {
          return finish(*line_end);
        }
        scanner.lines_remaining = static_cast<size_t>(*field_count);
        if (scanner.lines_remaining == 0) {
          return finish(*line_end);
        }
        scanner.mode = ResponseScanner::Mode::kDebugLines;
        continue;
      }
      case ResponseScanner::Mode::kDebugLines: {
        const auto line_end = next_line(line);
        if (!line_end) {
          return std::nullopt;
        }
        --scanner.lines_remaining;
        if (scanner.lines_remaining == 0) {
          return finish(*line_end);
        }
        continue;
      }
      case ResponseScanner::Mode::kValue: {
        if (const auto length = ScanValueLength(buffer, scanner)) {
          return finish(*length);
        }
        return std::nullopt;
      }
      case ResponseScanner::Mode::kDone:
        return scanner.completed_length;
    }
  }
}

inline std::optional<size_t> CompleteResponseLength(const std::string& buffer, ResponseFrame frame = {}) {
  ResponseScanner scanner(frame);
  return ScanResponseLength(buffer, scanner);
}

/**
 * @brief Resolve a host given as either a hostname or a numeric IPv4 address.
 *
 * The host a caller supplies is documented as a hostname or a numeric address,
 * so resolution goes through getaddrinfo rather than a numeric-only parse:
 * "localhost" has to reach the server exactly as "127.0.0.1" does. The port
 * travels as a service string with AI_NUMERICSERV so no name lookup is
 * attempted for it.
 *
 * @param host Hostname or numeric IPv4 address.
 * @param port TCP port.
 * @param candidates Receives the resolved address list, which the caller frees
 *        with freeaddrinfo(). Only written when this function succeeds.
 * @return std::nullopt on success, otherwise the resolution failure message.
 */
inline std::optional<std::string> ResolveHostV4(const std::string& host, uint16_t port, struct addrinfo** candidates) {
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;

  const std::string service = std::to_string(port);
  const int status = ::getaddrinfo(host.c_str(), service.c_str(), &hints, candidates);
  if (status != 0) {
    return "Failed to resolve host " + host + ": " + gai_strerror(status);
  }
  if (*candidates == nullptr) {
    return "Failed to resolve host " + host + ": no address returned";
  }
  return std::nullopt;
}

/**
 * @brief Whether a receive error means the deadline expired rather than the
 *        connection failing.
 *
 * SO_RCVTIMEO expiry surfaces as EAGAIN/EWOULDBLOCK on some platforms and as
 * ETIMEDOUT on others. All of them leave the connection intact, which is what
 * separates a retryable timeout from a genuine receive failure. Every client
 * surface classifies from this one predicate: recognising a different set in
 * one of them makes them disagree about what happened on the same socket.
 */
inline bool IsReceiveTimeout(int error_code) {
  return error_code == EAGAIN || error_code == EWOULDBLOCK || error_code == ETIMEDOUT;
}

inline struct timeval MakeSocketTimeout(uint32_t timeout_ms) {
  struct timeval value = {};
  value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout_ms / 1000U);
  value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout_ms % 1000U) * 1000U);
  return value;
}

/// Set the receive deadline. Failure is non-critical: the socket just keeps its previous deadline.
inline void SetReceiveTimeout(int socket_fd, uint32_t timeout_ms) {
  const struct timeval value = MakeSocketTimeout(timeout_ms);
  (void)::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
}

/// Set the receive and send deadlines to the same value.
inline void ApplySocketTimeouts(int socket_fd, uint32_t timeout_ms) {
  SetReceiveTimeout(socket_fd, timeout_ms);
  const struct timeval value = MakeSocketTimeout(timeout_ms);
  (void)::setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
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

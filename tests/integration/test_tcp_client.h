/**
 * @file test_tcp_client.h
 * @brief Shared TCP client helper for integration tests
 */

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Reply framing shared by the integration TCP client
 *
 * Mirrors the length-aware framing of the shipped client: END-terminated
 * blocks, `OK RESULTS <n>` result sets (plus the DEBUG trailer), RESP bulk and
 * array values, and single-line replies. Each function returns the byte length
 * of the first complete reply in `data`, or nullopt when more bytes are needed.
 */
namespace test_tcp_framing {

inline std::optional<size_t> LineEnd(const std::string& data, size_t start) {
  const size_t pos = data.find('\n', start);
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  return pos + 1;
}

inline std::string Line(const std::string& data, size_t start, size_t end) {
  std::string line = data.substr(start, end - start - 1);
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

inline std::optional<long> Decimal(const std::string& text) {
  if (text.empty()) {
    return std::nullopt;
  }
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (*end != '\0') {
    return std::nullopt;
  }
  return value;
}

inline bool StartsWith(const std::string& text, const std::string& prefix) {
  return text.compare(0, prefix.size(), prefix) == 0;
}

inline std::optional<size_t> CompleteValueLength(const std::string& data, size_t start, int depth = 0) {
  const auto line_end = LineEnd(data, start);
  if (!line_end) {
    return std::nullopt;
  }
  const std::string header = Line(data, start, *line_end);
  if (header.empty() || depth > 8) {
    return line_end;
  }
  if (header[0] == '$') {
    const auto length = Decimal(header.substr(1));
    if (!length || *length < 0) {
      return line_end;
    }
    const size_t payload_end = *line_end + static_cast<size_t>(*length);
    if (payload_end >= data.size()) {
      return std::nullopt;
    }
    if (data.compare(payload_end, 2, "\r\n") == 0) {
      return payload_end + 2;
    }
    if (data[payload_end] == '\n') {
      return payload_end + 1;
    }
    return line_end;
  }
  if (header[0] == '*') {
    const auto count = Decimal(header.substr(1));
    if (!count || *count < 0) {
      return line_end;
    }
    size_t cursor = *line_end;
    for (long i = 0; i < *count; ++i) {
      const auto next = CompleteValueLength(data, cursor, depth + 1);
      if (!next) {
        return std::nullopt;
      }
      cursor = *next;
    }
    return cursor;
  }
  return line_end;
}

inline std::optional<size_t> CompleteResponseLength(const std::string& data, const std::string& command,
                                                    bool debug_mode) {
  const auto first_end = LineEnd(data, 0);
  if (!first_end) {
    return std::nullopt;
  }
  const std::string header = Line(data, 0, *first_end);
  const bool is_error = StartsWith(header, "ERROR");

  std::string upper;
  const size_t begin = command.find_first_not_of(" \t");
  const size_t last = command.find_last_not_of(" \t");
  if (begin != std::string::npos) {
    upper = command.substr(begin, last - begin + 1);
  }
  std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });

  static const char* const kEndTerminated[] = {"INFO", "CONFIG", "CACHE STATS", "DUMP INFO", "DUMP STATUS"};
  bool end_terminated = false;
  for (const char* prefix : kEndTerminated) {
    const std::string name(prefix);
    if (upper == name || StartsWith(upper, name + " ")) {
      end_terminated = true;
    }
  }
  if (end_terminated && !is_error) {
    for (const char* terminator : {"\nEND\r\n", "\nEND\n"}) {
      const size_t pos = data.find(terminator);
      if (pos != std::string::npos) {
        return pos + std::string(terminator).size();
      }
    }
    return std::nullopt;
  }

  if (StartsWith(header, "OK RESULTS ")) {
    const auto count = Decimal(header.substr(std::string("OK RESULTS ").size()));
    if (!count || *count < 0) {
      return first_end;
    }
    size_t cursor = *first_end;
    for (long i = 0; i < *count; ++i) {
      const auto next = LineEnd(data, cursor);
      if (!next) {
        return std::nullopt;
      }
      cursor = *next;
    }
    if (!debug_mode) {
      return cursor;
    }
    const auto debug_end = LineEnd(data, cursor);
    if (!debug_end) {
      return std::nullopt;
    }
    const std::string debug_header = Line(data, cursor, *debug_end);
    if (!StartsWith(debug_header, "# DEBUG ")) {
      return debug_end;
    }
    const auto fields = Decimal(debug_header.substr(std::string("# DEBUG ").size()));
    if (!fields || *fields < 0) {
      return debug_end;
    }
    cursor = *debug_end;
    for (long i = 0; i < *fields; ++i) {
      const auto next = LineEnd(data, cursor);
      if (!next) {
        return std::nullopt;
      }
      cursor = *next;
    }
    return cursor;
  }
  return CompleteValueLength(data, 0);
}

}  // namespace test_tcp_framing

/**
 * @brief Simple TCP client for integration tests
 *
 * SendCommand returns exactly one complete reply however the server's bytes
 * are segmented; bytes past the reply stay buffered for the next command.
 */
class TcpClient {
 public:
  TcpClient(const std::string& host, uint16_t port) {
    // Ignore SIGPIPE globally (safe for test processes)
    signal(SIGPIPE, SIG_IGN);  // NOLINT(cert-err33-c)

    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
      throw std::runtime_error("Failed to create socket");
    }

    struct sockaddr_in server_addr {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr);

    if (connect(sock_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
      close(sock_);
      throw std::runtime_error("Failed to connect");
    }

    // Set recv timeout to avoid blocking indefinitely
    struct timeval tv {};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  ~TcpClient() { Close(); }

  // Non-copyable
  TcpClient(const TcpClient&) = delete;
  TcpClient& operator=(const TcpClient&) = delete;

  void Close() {
    if (sock_ >= 0) {
      close(sock_);
      sock_ = -1;
    }
  }

  bool IsConnected() const { return sock_ >= 0; }

  std::string SendCommand(const std::string& command) {
    std::string request = command + "\r\n";
    send(sock_, request.c_str(), request.length(), 0);

    while (true) {
      const auto length = test_tcp_framing::CompleteResponseLength(buffer_, command, debug_mode_);
      if (length) {
        std::string response = buffer_.substr(0, *length);
        buffer_.erase(0, *length);
        if (command == "DEBUG ON" && response.compare(0, 2, "OK") == 0) {
          debug_mode_ = true;
        } else if (command == "DEBUG OFF" && response.compare(0, 2, "OK") == 0) {
          debug_mode_ = false;
        }
        return response;
      }
      char chunk[65536];
      ssize_t received = recv(sock_, chunk, sizeof(chunk), 0);
      if (received <= 0) {
        // Closed or timed out: hand back whatever arrived so the caller can fail on it.
        std::string partial = std::move(buffer_);
        buffer_.clear();
        return partial;
      }
      buffer_.append(chunk, static_cast<size_t>(received));
    }
  }

 private:
  int sock_ = -1;
  std::string buffer_;
  bool debug_mode_ = false;
};

/**
 * @brief Parse SIM/SIMV results into id/score pairs
 * Format: "OK RESULTS N\r\nid1 score1\r\nid2 score2\r\n"
 */
inline std::vector<std::pair<std::string, float>> ParseSimResults(const std::string& response) {
  std::vector<std::pair<std::string, float>> results;
  std::istringstream stream(response);
  std::string line;

  // Skip "OK RESULTS N" header line
  if (!std::getline(stream, line)) {
    return results;
  }

  // Parse "id score" lines
  while (std::getline(stream, line)) {
    // Remove \r if present
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    auto space_pos = line.find(' ');
    if (space_pos != std::string::npos) {
      std::string id = line.substr(0, space_pos);
      float score = std::stof(line.substr(space_pos + 1));
      results.emplace_back(id, score);
    }
  }
  return results;
}

/**
 * @brief Get result count from SIM response
 */
inline int GetResultCount(const std::string& response) {
  // Format: "OK RESULTS N\r\n..."
  auto pos = response.find("RESULTS ");
  if (pos == std::string::npos) {
    return -1;
  }
  pos += 8;  // Skip "RESULTS "
  auto end_pos = response.find('\r', pos);
  if (end_pos == std::string::npos) {
    end_pos = response.find('\n', pos);
  }
  if (end_pos == std::string::npos) {
    return -1;
  }
  return std::stoi(response.substr(pos, end_pos - pos));
}

/**
 * @brief Extract a field value from an INFO or CACHE STATS response
 * Looks for "key: value" pattern and returns the value string
 */
inline std::string ParseResponseField(const std::string& response, const std::string& key) {
  std::string search = key + ": ";
  auto pos = response.find(search);
  if (pos == std::string::npos) {
    search = key + ":";
    pos = response.find(search);
    if (pos == std::string::npos) {
      return "";
    }
  }
  pos += search.length();
  auto end_pos = response.find('\n', pos);
  if (end_pos == std::string::npos) {
    end_pos = response.length();
  }
  std::string value = response.substr(pos, end_pos - pos);
  // Trim \r
  if (!value.empty() && value.back() == '\r') {
    value.pop_back();
  }
  // Trim leading space
  if (!value.empty() && value.front() == ' ') {
    value = value.substr(1);
  }
  return value;
}

inline bool ContainsOK(const std::string& response) {
  return response.find("OK") == 0;
}

/// True when the reply is an error line; the wire spelling is an `ERROR` prefix.
inline bool ContainsError(const std::string& response) {
  return response.compare(0, 5, "ERROR") == 0;
}

/**
 * @file test_tcp_client.h
 * @brief Shared TCP client helper for integration tests
 */

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Simple TCP client for integration tests
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

    char buffer[65536];
    ssize_t received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
      return "";
    }
    buffer[received] = '\0';
    return std::string(buffer);
  }

 private:
  int sock_ = -1;
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

inline bool ContainsError(const std::string& response) {
  return response.find("ERROR") != std::string::npos || response.find("ERR") != std::string::npos;
}

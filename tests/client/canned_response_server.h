/**
 * @file canned_response_server.h
 * @brief Minimal TCP peer that answers each received line with a scripted reply
 *
 * Some wire responses only appear under a server configuration that is
 * awkward to drive from a unit test (the asynchronous DUMP SAVE reply needs a
 * forking snapshot writer, for example). Serving the exact bytes lets the
 * client-side parsing of those responses be pinned deterministically.
 */

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nvecd::testing {

/**
 * @brief TCP listener that replies to each request line from a fixed script
 *
 * Accepts a single connection. The n-th complete line received is answered
 * with the n-th scripted reply; once the script is exhausted the last reply is
 * repeated. The peer must disconnect before the server is destroyed.
 */
class CannedResponseServer {
 public:
  explicit CannedResponseServer(std::vector<std::string> replies) : replies_(std::move(replies)) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) {
      return;
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Required for socket API
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener_, 1) != 0) {
      ::close(listener_);
      listener_ = -1;
      return;
    }
    socklen_t address_length = sizeof(address);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Required for socket API
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &address_length);
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this]() { Serve(); });
  }

  ~CannedResponseServer() {
    if (worker_.joinable()) {
      worker_.join();
    }
    if (listener_ >= 0) {
      ::close(listener_);
    }
  }

  CannedResponseServer(const CannedResponseServer&) = delete;
  CannedResponseServer& operator=(const CannedResponseServer&) = delete;
  CannedResponseServer(CannedResponseServer&&) = delete;
  CannedResponseServer& operator=(CannedResponseServer&&) = delete;

  [[nodiscard]] uint16_t Port() const { return port_; }

 private:
  void Serve() {
    if (replies_.empty()) {
      return;
    }
    const int connection = ::accept(listener_, nullptr, nullptr);
    if (connection < 0) {
      return;
    }
    std::string pending;
    size_t reply_index = 0;
    while (true) {
      char buffer[512] = {};
      const ssize_t received = ::recv(connection, buffer, sizeof(buffer), 0);
      if (received <= 0) {
        break;
      }
      pending.append(buffer, static_cast<size_t>(received));
      size_t line_end = pending.find('\n');
      while (line_end != std::string::npos) {
        pending.erase(0, line_end + 1);
        const std::string& reply = replies_[reply_index < replies_.size() ? reply_index : replies_.size() - 1];
        ++reply_index;
        if (::send(connection, reply.data(), reply.size(), 0) < 0) {
          ::close(connection);
          return;
        }
        line_end = pending.find('\n');
      }
    }
    ::close(connection);
  }

  std::vector<std::string> replies_;
  int listener_ = -1;
  uint16_t port_ = 0;
  std::thread worker_;
};

}  // namespace nvecd::testing

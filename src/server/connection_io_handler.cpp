/**
 * @file connection_io_handler.cpp
 * @brief Network I/O handler implementation
 */

#include "server/connection_io_handler.h"

#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "server/server_types.h"
#include "utils/structured_log.h"

namespace nvecd::server {

ConnectionIOHandler::ConnectionIOHandler(const IOConfig& config, RequestProcessor processor,
                                         const std::atomic<bool>& shutdown_flag)
    : config_(config), processor_(std::move(processor)), shutdown_flag_(shutdown_flag) {}

void ConnectionIOHandler::HandleConnection(int client_fd, ConnectionContext& ctx) {
  // Set receive timeout on the socket if configured
  if (config_.recv_timeout_sec > 0) {
    struct timeval timeout {};  // Zero-initialized to avoid uninitialized warning
    timeout.tv_sec = config_.recv_timeout_sec;
    timeout.tv_usec = 0;
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
      nvecd::utils::StructuredLog()
          .Event("server_warning")
          .Field("operation", "setsockopt")
          .Field("option", "SO_RCVTIMEO")
          .Field("fd", static_cast<uint64_t>(client_fd))
          .Field("error", strerror(errno))
          .Warn();
      // Continue anyway - timeout is not critical for functionality
    }
  }

  std::vector<char> buffer(config_.recv_buffer_size);
  std::string accumulated;
  const size_t max_accumulated = config_.max_accumulated_bytes;

  while (!shutdown_flag_) {
    ssize_t bytes = recv(client_fd, buffer.data(), buffer.size() - 1, 0);

    if (bytes <= 0) {
      if (bytes < 0) {
        // With SO_RCVTIMEO set, timeout will trigger EAGAIN/EWOULDBLOCK
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          spdlog::debug("recv timeout on fd {}, closing connection", client_fd);
          break;  // Timeout - close connection
        }
        spdlog::debug("recv error on fd {}: {}", client_fd, strerror(errno));
      } else {
        spdlog::debug("recv returned 0 on fd {} (client closed connection)", client_fd);
      }
      break;
    }

    // Check buffer size limit
    if (accumulated.size() + bytes > max_accumulated) {
      nvecd::utils::StructuredLog()
          .Event("server_warning")
          .Field("type", "request_too_large")
          .Field("fd", static_cast<uint64_t>(client_fd))
          .Field("size", static_cast<uint64_t>(accumulated.size() + bytes))
          .Field("limit", static_cast<uint64_t>(max_accumulated))
          .Warn();
      SendResponse(client_fd, "ERROR Request too large (no newline detected)");
      break;
    }

    // Preserve every received byte. Using the C-string overload would silently
    // truncate at an embedded NUL before the command parser can reject it.
    accumulated.append(buffer.data(), static_cast<size_t>(bytes));

    // Process complete requests
    if (!ProcessBuffer(accumulated, client_fd, ctx)) {
      break;
    }
  }
}

bool ConnectionIOHandler::ProcessBuffer(std::string& accumulated, int client_fd, ConnectionContext& ctx) {
  while (true) {
    // Support both \r\n and \n line endings (protocol spec line 19)
    size_t pos = accumulated.find("\r\n");
    size_t delimiter_len = 2;

    if (pos == std::string::npos) {
      pos = accumulated.find('\n');
      delimiter_len = 1;
    }

    if (pos == std::string::npos) {
      break;  // No complete line found
    }

    if (pos > config_.max_query_length) {
      nvecd::utils::StructuredLog()
          .Event("server_warning")
          .Field("type", "request_too_large")
          .Field("fd", static_cast<uint64_t>(client_fd))
          .Field("size", static_cast<uint64_t>(pos))
          .Field("limit", static_cast<uint64_t>(config_.max_query_length))
          .Warn();
      SendResponse(client_fd, "ERROR Request too large");
      return false;
    }

    std::string request = accumulated.substr(0, pos);
    accumulated = accumulated.substr(pos + delimiter_len);

    if (request.empty()) {
      continue;
    }

    // Process request
    std::string response = processor_(request, ctx);

    // Send response
    if (!SendResponse(client_fd, response)) {
      return false;
    }
  }

  return true;
}

std::string ConnectionIOHandler::NormalizeResponse(const std::string& response) {
  // Canonical framing policy: every line is terminated with CRLF and the
  // response carries exactly one trailing CRLF. Handler bodies historically
  // mixed bare "\n" (between lines and as a trailing terminator) with the CRLF
  // that SendResponse used to append unconditionally. That produced an extra
  // blank line on single-line responses and mis-framed multi-line responses for
  // clients that split on CRLF. Normalizing here keeps the policy in one place,
  // independent of how each handler builds its body.
  //
  // Strip any trailing newline characters first so we do not emit a blank
  // terminating line, then rewrite every interior bare "\n" to "\r\n".
  size_t end = response.size();
  while (end > 0 && (response[end - 1] == '\n' || response[end - 1] == '\r')) {
    --end;
  }

  std::string normalized;
  normalized.reserve(end + 2);
  for (size_t i = 0; i < end; ++i) {
    char ch = response[i];
    if (ch == '\r') {
      // Skip a CR; the following LF (or this position) becomes a CRLF below.
      continue;
    }
    if (ch == '\n') {
      normalized += "\r\n";
      continue;
    }
    normalized += ch;
  }
  normalized += "\r\n";
  return normalized;
}

// Kept as member function for consistency and potential future extensions
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool ConnectionIOHandler::SendResponse(int client_fd, const std::string& response) {
  // Normalize line endings once, then send the framed buffer.
  const std::string framed = NormalizeResponse(response);

  size_t total_sent = 0;
  while (total_sent < framed.size()) {
    ssize_t sent = send(client_fd, framed.data() + total_sent, framed.size() - total_sent, 0);

    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno != EPIPE) {
        spdlog::debug("send error on fd {}: {}", client_fd, strerror(errno));
      }
      return false;
    }

    if (sent == 0) {
      spdlog::debug("send returned 0 on fd {}", client_fd);
      return false;
    }

    total_sent += static_cast<size_t>(sent);
  }

  return true;
}

}  // namespace nvecd::server

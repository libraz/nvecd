/**
 * @file connection_acceptor.cpp
 * @brief Implementation of ConnectionAcceptor
 */

#include "server/connection_acceptor.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

#include "server/server_types.h"
#include "server/thread_pool.h"
#include "utils/error.h"
#include "utils/expected.h"
#include "utils/network_utils.h"
#include "utils/structured_log.h"

namespace nvecd::server {

namespace {
/**
 * @brief Helper to safely cast sockaddr_in* to sockaddr* for socket API
 *
 * POSIX socket API requires sockaddr* but we use sockaddr_in for IPv4.
 * This helper centralizes the required reinterpret_cast to a single location.
 */
inline struct sockaddr* ToSockaddr(struct sockaddr_in* addr) {
  return reinterpret_cast<struct sockaddr*>(addr);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

/**
 * @brief Helper to safely cast sockaddr_un* to sockaddr* for socket API
 *
 * POSIX socket API requires sockaddr* but we use sockaddr_un for Unix domain sockets.
 * This helper centralizes the required reinterpret_cast to a single location.
 */
inline struct sockaddr* ToSockaddrUn(struct sockaddr_un* addr) {
  return reinterpret_cast<struct sockaddr*>(addr);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}
}  // namespace

ConnectionAcceptor::ConnectionAcceptor(ServerConfig config, ThreadPool* thread_pool)
    : config_(std::move(config)), thread_pool_(thread_pool) {
  if (thread_pool_ == nullptr) {
    nvecd::utils::StructuredLog()
        .Event("server_error")
        .Field("component", "connection_acceptor")
        .Field("error", "thread_pool cannot be null")
        .Error();
  }
}

ConnectionAcceptor::~ConnectionAcceptor() {
  Stop();
}

nvecd::utils::Expected<void, nvecd::utils::Error> ConnectionAcceptor::Start() {
  using nvecd::utils::ErrorCode;
  using nvecd::utils::MakeError;
  using nvecd::utils::MakeUnexpected;

  if (running_) {
    auto error = MakeError(ErrorCode::kNetworkAlreadyRunning, "ConnectionAcceptor already running");
    nvecd::utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "connection_acceptor_start")
        .Field("error", error.to_string())
        .Error();
    return MakeUnexpected(error);
  }

  // Unix domain socket mode
  if (!config_.unix_socket_path.empty()) {
    unix_socket_path_ = config_.unix_socket_path;

    // Validate path length
    struct sockaddr_un addr_check {};
    if (unix_socket_path_.size() >= sizeof(addr_check.sun_path)) {
      return MakeUnexpected(MakeError(ErrorCode::kNetworkUnixSocketPathTooLong,
                                      "Unix socket path too long (max " +
                                          std::to_string(sizeof(addr_check.sun_path) - 1) + "): " + unix_socket_path_));
    }

    // Check for stale socket file
    if (access(unix_socket_path_.c_str(), F_OK) == 0) {
      // File exists -- probe to check if another server is listening
      int probe_fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (probe_fd >= 0) {
        struct sockaddr_un probe_addr {};
        probe_addr.sun_family = AF_UNIX;
        std::strncpy(probe_addr.sun_path, unix_socket_path_.c_str(), sizeof(probe_addr.sun_path) - 1);

        if (connect(probe_fd, ToSockaddrUn(&probe_addr), sizeof(probe_addr)) == 0) {
          // Connection succeeded -- another server is active
          close(probe_fd);
          return MakeUnexpected(MakeError(ErrorCode::kNetworkUnixSocketStale,
                                          "Another server is already listening on: " + unix_socket_path_));
        }
        close(probe_fd);
      }

      // Stale socket file -- remove it
      unlink(unix_socket_path_.c_str());
      nvecd::utils::StructuredLog().Event("unix_socket_stale_removed").Field("path", unix_socket_path_).Info();
    }

    // Create unix socket
    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
      return MakeUnexpected(MakeError(ErrorCode::kNetworkSocketCreationFailed,
                                      "Failed to create unix socket: " + std::string(strerror(errno))));
    }

    // Bind
    struct sockaddr_un bind_addr {};
    bind_addr.sun_family = AF_UNIX;
    std::strncpy(bind_addr.sun_path, unix_socket_path_.c_str(), sizeof(bind_addr.sun_path) - 1);

    if (bind(server_fd_, ToSockaddrUn(&bind_addr), sizeof(bind_addr)) < 0) {
      close(server_fd_);
      server_fd_ = -1;
      return MakeUnexpected(
          MakeError(ErrorCode::kNetworkBindFailed, "Failed to bind unix socket: " + std::string(strerror(errno))));
    }

    // Set permissions (owner + group only)
    // NOLINTNEXTLINE(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
    if (chmod(unix_socket_path_.c_str(), 0770) < 0) {
      nvecd::utils::StructuredLog()
          .Event("unix_socket_chmod_warning")
          .Field("path", unix_socket_path_)
          .Field("error", strerror(errno))
          .Warn();
    }

    // Listen
    if (listen(server_fd_, config_.max_connections) < 0) {
      close(server_fd_);
      server_fd_ = -1;
      unlink(unix_socket_path_.c_str());
      return MakeUnexpected(MakeError(ErrorCode::kNetworkListenFailed,
                                      "Failed to listen on unix socket: " + std::string(strerror(errno))));
    }

    actual_port_ = 0;  // UDS has no port

    // Start accept thread
    should_stop_.store(false);
    running_ = true;
    accept_thread_ = std::make_unique<std::thread>(&ConnectionAcceptor::AcceptLoop, this);

    nvecd::utils::StructuredLog().Event("unix_socket_listening").Field("path", unix_socket_path_).Info();
    return {};  // Early return -- skip TCP code below
  }

  // Create socket
  server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    auto error =
        MakeError(ErrorCode::kNetworkSocketCreationFailed, "Failed to create socket: " + std::string(strerror(errno)));
    nvecd::utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "socket_create")
        .Field("error", error.to_string())
        .Error();
    return MakeUnexpected(error);
  }

  // Set socket options
  if (!SetSocketOptions(server_fd_)) {
    close(server_fd_);
    server_fd_ = -1;
    auto error = MakeError(ErrorCode::kNetworkSocketCreationFailed, "Failed to set socket options");
    nvecd::utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "socket_set_options")
        .Field("error", error.to_string())
        .Error();
    return MakeUnexpected(error);
  }

  // Bind
  struct sockaddr_in address = {};
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(config_.port);

  // Determine bind address (default to 0.0.0.0 for backward compatibility)
  in_addr bind_addr{};
  if (config_.host.empty() || config_.host == "0.0.0.0") {
    bind_addr.s_addr = INADDR_ANY;
  } else {
    if (inet_pton(AF_INET, config_.host.c_str(), &bind_addr) != 1) {
      close(server_fd_);
      server_fd_ = -1;
      auto error = MakeError(ErrorCode::kNetworkInvalidBindAddress, "Invalid bind address: " + config_.host);
      nvecd::utils::StructuredLog()
          .Event("server_error")
          .Field("operation", "socket_bind")
          .Field("bind_address", config_.host)
          .Field("error", error.to_string())
          .Error();
      return MakeUnexpected(error);
    }
  }
  address.sin_addr = bind_addr;

  if (bind(server_fd_, ToSockaddr(&address), sizeof(address)) < 0) {
    close(server_fd_);
    server_fd_ = -1;
    auto error = MakeError(ErrorCode::kNetworkBindFailed, "Failed to bind to port " + std::to_string(config_.port) +
                                                              ": " + std::string(strerror(errno)));
    nvecd::utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "socket_bind")
        .Field("port", static_cast<uint64_t>(config_.port))
        .Field("error", error.to_string())
        .Error();
    return MakeUnexpected(error);
  }

  // Get actual port if port 0 was specified
  if (config_.port == 0) {
    socklen_t addr_len = sizeof(address);
    if (getsockname(server_fd_, ToSockaddr(&address), &addr_len) == 0) {
      actual_port_ = ntohs(address.sin_port);
    }
  } else {
    actual_port_ = config_.port;
  }

  // Listen
  if (listen(server_fd_, config_.max_connections) < 0) {
    close(server_fd_);
    server_fd_ = -1;
    auto error = MakeError(ErrorCode::kNetworkListenFailed, "Failed to listen: " + std::string(strerror(errno)));
    nvecd::utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "socket_listen")
        .Field("error", error.to_string())
        .Error();
    return MakeUnexpected(error);
  }

  should_stop_ = false;
  running_ = true;

  // Start accept thread
  accept_thread_ = std::make_unique<std::thread>(&ConnectionAcceptor::AcceptLoop, this);

  spdlog::info("ConnectionAcceptor listening on {}:{}", config_.host, actual_port_);
  return {};
}

void ConnectionAcceptor::Stop() {
  if (!running_) {
    return;
  }

  spdlog::info("Stopping ConnectionAcceptor...");
  should_stop_ = true;
  running_ = false;

  // Close server socket to unblock accept()
  if (server_fd_ >= 0) {
    shutdown(server_fd_, SHUT_RDWR);
    close(server_fd_);
    server_fd_ = -1;
  }

  // Wait for accept thread to finish
  if (accept_thread_ && accept_thread_->joinable()) {
    accept_thread_->join();
  }

  // Remove unix socket file
  if (!unix_socket_path_.empty()) {
    unlink(unix_socket_path_.c_str());
    nvecd::utils::StructuredLog().Event("unix_socket_removed").Field("path", unix_socket_path_).Debug();
    unix_socket_path_.clear();
  }

  // Close all active connections
  {
    std::lock_guard<std::mutex> lock(fds_mutex_);
    for (int socket_fd : active_fds_) {
      // Shutdown socket to unblock recv/send calls in other threads
      shutdown(socket_fd, SHUT_RDWR);
      close(socket_fd);
    }
    active_fds_.clear();
  }

  spdlog::info("ConnectionAcceptor stopped");
}

void ConnectionAcceptor::SetConnectionHandler(ConnectionHandler handler) {
  connection_handler_ = std::move(handler);
}

void ConnectionAcceptor::AcceptLoop() {
  spdlog::info("Accept loop started");

  while (!should_stop_) {
    int client_fd = -1;
    std::string client_ip;

    if (IsUnixSocket()) {
      struct sockaddr_un client_addr_un {};
      socklen_t client_len_un = sizeof(client_addr_un);
      client_fd = accept(server_fd_, ToSockaddrUn(&client_addr_un), &client_len_un);
      client_ip = "unix";  // UDS connections have no IP
    } else {
      struct sockaddr_in client_addr = {};
      socklen_t client_len = sizeof(client_addr);
      client_fd = accept(server_fd_, ToSockaddr(&client_addr), &client_len);

      // Convert client IP to string for ACL checks
      // C-style array required by POSIX inet_ntop API
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
      char ip_buffer[INET_ADDRSTRLEN] = {};
      // Array-to-pointer decay required by POSIX inet_ntop API
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
      if (inet_ntop(AF_INET, &client_addr.sin_addr, ip_buffer, sizeof(ip_buffer)) != nullptr) {
        // Array-to-pointer decay required by std::string::assign
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
        client_ip.assign(ip_buffer);
      } else {
        nvecd::utils::StructuredLog().Event("server_warning").Field("type", "client_address_parse_failed").Warn();
      }
    }

    if (client_fd < 0) {
      if (!should_stop_) {
        nvecd::utils::StructuredLog()
            .Event("server_error")
            .Field("operation", "accept")
            .Field("error", strerror(errno))
            .Error();
      } else {
        spdlog::debug("Accept interrupted (shutdown in progress)");
      }
      continue;
    }

    // SECURITY: Check connection limit BEFORE any processing to prevent resource exhaustion
    {
      std::lock_guard<std::mutex> lock(fds_mutex_);
      if (static_cast<int>(active_fds_.size()) >= config_.max_connections) {
        nvecd::utils::StructuredLog()
            .Event("server_warning")
            .Field("type", "connection_limit_reached")
            .Field("active_connections", static_cast<uint64_t>(active_fds_.size()))
            .Field("max_connections", static_cast<uint64_t>(config_.max_connections))
            .Warn();
        close(client_fd);
        continue;
      }
    }

    // ACL check (skip for Unix domain sockets -- filesystem permissions apply)
    if (!IsUnixSocket()) {
      if (!utils::IsIPAllowed(client_ip, config_.parsed_allow_cidrs)) {
        nvecd::utils::StructuredLog()
            .Event("server_warning")
            .Field("type", "connection_rejected_acl")
            .Field("client_ip", client_ip.empty() ? "<unknown>" : client_ip)
            .Warn();
        close(client_fd);
        continue;
      }
    }

    // Set receive timeout to avoid blocking indefinitely
    struct timeval timeout {};
    timeout.tv_sec = 1;  // 1 second timeout (short for quick shutdown)
    timeout.tv_usec = 0;
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
      nvecd::utils::StructuredLog()
          .Event("server_warning")
          .Field("type", "setsockopt_failed")
          .Field("option", "SO_RCVTIMEO")
          .Field("error", strerror(errno))
          .Warn();
    }

    // Track connection
    {
      std::lock_guard<std::mutex> lock(fds_mutex_);
      active_fds_.insert(client_fd);
    }

    // Submit to thread pool
    if (thread_pool_ != nullptr && connection_handler_) {
      bool submitted = thread_pool_->Submit([this, client_fd]() {
        connection_handler_(client_fd);
        RemoveConnection(client_fd);
      });

      if (!submitted) {
        // Queue is full - reject connection to prevent FD leak
        nvecd::utils::StructuredLog()
            .Event("server_warning")
            .Field("type", "thread_pool_queue_full")
            .Field("client_fd", static_cast<uint64_t>(client_fd))
            .Warn();
        close(client_fd);
        RemoveConnection(client_fd);
      }
    } else {
      nvecd::utils::StructuredLog()
          .Event("server_error")
          .Field("type", "no_connection_handler")
          .Field("error", "No connection handler or thread pool configured")
          .Error();
      close(client_fd);
      RemoveConnection(client_fd);
    }
  }

  spdlog::info("Accept loop exited");
}

bool ConnectionAcceptor::SetSocketOptions(int socket_fd) const {
  // SO_REUSEADDR: Allow reuse of local addresses
  int opt = 1;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    std::string error_msg = "Failed to set SO_REUSEADDR: " + std::string(strerror(errno));
    nvecd::utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "setsockopt")
        .Field("option", "SO_REUSEADDR")
        .Field("error", error_msg)
        .Error();
    return false;
  }

  // SO_KEEPALIVE: Enable TCP keepalive
  if (setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0) {
    std::string error_msg = "Failed to set SO_KEEPALIVE: " + std::string(strerror(errno));
    nvecd::utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "setsockopt")
        .Field("option", "SO_KEEPALIVE")
        .Field("error", error_msg)
        .Error();
    return false;
  }

  // SO_RCVBUF: Set receive buffer size
  int rcvbuf = config_.recv_buffer_size;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
    nvecd::utils::StructuredLog()
        .Event("server_warning")
        .Field("operation", "setsockopt")
        .Field("option", "SO_RCVBUF")
        .Field("error", strerror(errno))
        .Warn();
    // Non-fatal, continue
  }

  // SO_SNDBUF: Set send buffer size
  int sndbuf = config_.send_buffer_size;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
    nvecd::utils::StructuredLog()
        .Event("server_warning")
        .Field("operation", "setsockopt")
        .Field("option", "SO_SNDBUF")
        .Field("error", strerror(errno))
        .Warn();
    // Non-fatal, continue
  }

  return true;
}

void ConnectionAcceptor::RemoveConnection(int socket_fd) {
  std::lock_guard<std::mutex> lock(fds_mutex_);
  active_fds_.erase(socket_fd);
}

}  // namespace nvecd::server

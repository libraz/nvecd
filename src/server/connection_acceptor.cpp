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

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

#include "server/server_types.h"
#include "server/thread_pool.h"
#include "utils/error.h"
#include "utils/expected.h"
#include "utils/network_utils.h"
#include "utils/path_utils.h"
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
    const std::filesystem::path configured_path(config_.unix_socket_path);
    const auto parent =
        configured_path.parent_path().empty() ? std::filesystem::path(".") : configured_path.parent_path();
    std::error_code path_error;
    const auto canonical_parent = std::filesystem::canonical(parent, path_error);
    if (path_error || configured_path.filename().empty()) {
      return MakeUnexpected(
          MakeError(ErrorCode::kNetworkUnixSocketStale, "Invalid Unix socket parent directory: " + parent.string()));
    }
    auto private_parent = nvecd::utils::ValidatePrivateDirectory(canonical_parent);
    if (!private_parent) {
      return MakeUnexpected(private_parent.error());
    }
    unix_socket_path_ = (canonical_parent / configured_path.filename()).string();
    unix_socket_filename_ = configured_path.filename().string();

    // Validate path length
    struct sockaddr_un addr_check {};
    if (unix_socket_path_.size() >= sizeof(addr_check.sun_path)) {
      return MakeUnexpected(MakeError(ErrorCode::kNetworkUnixSocketPathTooLong,
                                      "Unix socket path too long (max " +
                                          std::to_string(sizeof(addr_check.sun_path) - 1) + "): " + unix_socket_path_));
    }

    // Keep the validated parent inode open for all identity checks and unlink.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open requires mode only with O_CREAT.
    unix_socket_parent_fd_ = ::open(canonical_parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (unix_socket_parent_fd_ < 0) {
      return MakeUnexpected(MakeError(ErrorCode::kPermissionDenied, "Failed to securely open Unix socket directory: " +
                                                                        std::string(strerror(errno))));
    }

    // Check for a stale socket without following a replacement symlink. Never
    // remove an arbitrary regular file at the configured path.
    struct stat existing_info {};
    if (::fstatat(unix_socket_parent_fd_, unix_socket_filename_.c_str(), &existing_info, AT_SYMLINK_NOFOLLOW) == 0) {
      if (!S_ISSOCK(existing_info.st_mode) || existing_info.st_uid != ::geteuid()) {
        ::close(unix_socket_parent_fd_);
        unix_socket_parent_fd_ = -1;
        return MakeUnexpected(
            MakeError(ErrorCode::kNetworkUnixSocketStale,
                      "Refusing to replace non-socket or foreign-owned Unix socket path: " + unix_socket_path_));
      }
      // File exists -- probe to check if another server is listening
      int probe_fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (probe_fd >= 0) {
        struct sockaddr_un probe_addr {};
        probe_addr.sun_family = AF_UNIX;
        std::strncpy(probe_addr.sun_path, unix_socket_path_.c_str(), sizeof(probe_addr.sun_path) - 1);

        if (connect(probe_fd, ToSockaddrUn(&probe_addr), sizeof(probe_addr)) == 0) {
          // Connection succeeded -- another server is active
          close(probe_fd);
          ::close(unix_socket_parent_fd_);
          unix_socket_parent_fd_ = -1;
          return MakeUnexpected(MakeError(ErrorCode::kNetworkUnixSocketStale,
                                          "Another server is already listening on: " + unix_socket_path_));
        }
        close(probe_fd);
      }

      struct stat rechecked_info {};
      if (::fstatat(unix_socket_parent_fd_, unix_socket_filename_.c_str(), &rechecked_info, AT_SYMLINK_NOFOLLOW) != 0 ||
          rechecked_info.st_dev != existing_info.st_dev || rechecked_info.st_ino != existing_info.st_ino ||
          !S_ISSOCK(rechecked_info.st_mode) || rechecked_info.st_uid != ::geteuid() ||
          ::unlinkat(unix_socket_parent_fd_, unix_socket_filename_.c_str(), 0) != 0) {
        ::close(unix_socket_parent_fd_);
        unix_socket_parent_fd_ = -1;
        return MakeUnexpected(MakeError(ErrorCode::kNetworkUnixSocketStale,
                                        "Unix socket path changed while removing stale socket: " + unix_socket_path_));
      }
      nvecd::utils::StructuredLog().Event("unix_socket_stale_removed").Field("path", unix_socket_path_).Info();
    } else if (errno != ENOENT) {
      const int saved_errno = errno;
      ::close(unix_socket_parent_fd_);
      unix_socket_parent_fd_ = -1;
      return MakeUnexpected(MakeError(ErrorCode::kNetworkUnixSocketStale,
                                      "Failed to inspect Unix socket path: " + std::string(strerror(saved_errno))));
    }

    // Create unix socket
    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
      ::close(unix_socket_parent_fd_);
      unix_socket_parent_fd_ = -1;
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
      ::close(unix_socket_parent_fd_);
      unix_socket_parent_fd_ = -1;
      return MakeUnexpected(
          MakeError(ErrorCode::kNetworkBindFailed, "Failed to bind unix socket: " + std::string(strerror(errno))));
    }

    struct stat bound_info {};
    if (::fstatat(unix_socket_parent_fd_, unix_socket_filename_.c_str(), &bound_info, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISSOCK(bound_info.st_mode) || bound_info.st_uid != ::geteuid()) {
      close(server_fd_);
      server_fd_ = -1;
      (void)::unlinkat(unix_socket_parent_fd_, unix_socket_filename_.c_str(), 0);
      ::close(unix_socket_parent_fd_);
      unix_socket_parent_fd_ = -1;
      return MakeUnexpected(MakeError(ErrorCode::kNetworkBindFailed, "Bound Unix socket identity validation failed"));
    }
    unix_socket_device_ = bound_info.st_dev;
    unix_socket_inode_ = bound_info.st_ino;

    // Set permissions (owner + group only). The private parent prevents path
    // replacement, and identity is rechecked immediately afterwards.
    // NOLINTNEXTLINE(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
    if (chmod(unix_socket_path_.c_str(), 0770) < 0) {
      nvecd::utils::StructuredLog()
          .Event("unix_socket_chmod_warning")
          .Field("path", unix_socket_path_)
          .Field("error", strerror(errno))
          .Warn();
    }
    struct stat chmod_info {};
    if (::fstatat(unix_socket_parent_fd_, unix_socket_filename_.c_str(), &chmod_info, AT_SYMLINK_NOFOLLOW) != 0 ||
        chmod_info.st_dev != unix_socket_device_ || chmod_info.st_ino != unix_socket_inode_ ||
        !S_ISSOCK(chmod_info.st_mode)) {
      close(server_fd_);
      server_fd_ = -1;
      ::close(unix_socket_parent_fd_);
      unix_socket_parent_fd_ = -1;
      return MakeUnexpected(MakeError(ErrorCode::kNetworkBindFailed, "Unix socket changed during chmod"));
    }

    // Listen
    if (listen(server_fd_, config_.max_connections) < 0) {
      close(server_fd_);
      server_fd_ = -1;
      (void)::unlinkat(unix_socket_parent_fd_, unix_socket_filename_.c_str(), 0);
      ::close(unix_socket_parent_fd_);
      unix_socket_parent_fd_ = -1;
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
    struct stat current_info {};
    const bool same_socket =
        unix_socket_parent_fd_ >= 0 &&
        ::fstatat(unix_socket_parent_fd_, unix_socket_filename_.c_str(), &current_info, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISSOCK(current_info.st_mode) && current_info.st_uid == ::geteuid() &&
        current_info.st_dev == unix_socket_device_ && current_info.st_ino == unix_socket_inode_;
    if (same_socket && ::unlinkat(unix_socket_parent_fd_, unix_socket_filename_.c_str(), 0) == 0) {
      nvecd::utils::StructuredLog().Event("unix_socket_removed").Field("path", unix_socket_path_).Debug();
    } else if (!same_socket) {
      nvecd::utils::StructuredLog().Event("unix_socket_replacement_preserved").Field("path", unix_socket_path_).Warn();
    }
    if (unix_socket_parent_fd_ >= 0) {
      ::close(unix_socket_parent_fd_);
      unix_socket_parent_fd_ = -1;
    }
    unix_socket_path_.clear();
    unix_socket_filename_.clear();
    unix_socket_device_ = 0;
    unix_socket_inode_ = 0;
  }

  // In reactor mode, IoReactor owns the client fds and releases accounting
  // through RemoveConnection(). Closing here would race its event loop.
  if (!reactor_handler_) {
    std::lock_guard<std::mutex> lock(fds_mutex_);
    for (int socket_fd : active_fds_) {
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

void ConnectionAcceptor::SetReactorHandler(ReactorHandler handler) {
  reactor_handler_ = std::move(handler);
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
        if (errno == EMFILE || errno == ENFILE) {
          nvecd::utils::StructuredLog()
              .Event("server_error")
              .Field("operation", "accept_fd_exhaustion")
              .Field("error", strerror(errno))
              .Error();
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }
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

    SetClientSocketOptions(client_fd);

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

    // Per-IP connection limit check
    if (config_.max_connections_per_ip > 0 && !client_ip.empty() && client_ip != "unix") {
      std::lock_guard<std::mutex> lock(fds_mutex_);
      auto it = per_ip_connections_.find(client_ip);
      if (it != per_ip_connections_.end() && it->second >= config_.max_connections_per_ip) {
        nvecd::utils::StructuredLog()
            .Event("server_warning")
            .Field("type", "per_ip_limit_reached")
            .Field("client_ip", client_ip)
            .Field("connections", static_cast<uint64_t>(it->second))
            .Field("limit", static_cast<uint64_t>(config_.max_connections_per_ip))
            .Warn();
        close(client_fd);
        continue;
      }
    }

    // The legacy worker-per-connection handler uses a short receive timeout.
    // Reactor sockets are non-blocking and use IoReactor's idle reaper instead.
    if (!reactor_handler_) {
      struct timeval timeout {};
      timeout.tv_sec = 1;
      timeout.tv_usec = 0;
      if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        nvecd::utils::StructuredLog()
            .Event("server_warning")
            .Field("type", "setsockopt_failed")
            .Field("option", "SO_RCVTIMEO")
            .Field("error", strerror(errno))
            .Warn();
      }
    }

    // Track connection (global and per-IP)
    {
      std::lock_guard<std::mutex> lock(fds_mutex_);
      active_fds_.insert(client_fd);
      if (!client_ip.empty() && client_ip != "unix") {
        per_ip_connections_[client_ip]++;
        fd_to_ip_[client_fd] = client_ip;
      }
    }

    // Registration is intentionally done on the accept thread: it is a small
    // map insertion plus one poller call, and must not consume a worker for an
    // idle socket's lifetime.
    if (reactor_handler_) {
      if (!reactor_handler_(client_fd)) {
        close(client_fd);
        RemoveConnection(client_fd);
      }
    } else if (thread_pool_ != nullptr && connection_handler_) {
      bool submitted = thread_pool_->Submit([this, client_fd]() {
        connection_handler_(client_fd);
        RemoveConnection(client_fd);
        // The acceptor owns the accepted fd's lifecycle; the connection handler
        // and I/O handler never close it. Close here so a completed connection
        // releases its fd (otherwise every served connection leaks one fd,
        // eventually exhausting RLIMIT_NOFILE and denying all service).
        close(client_fd);
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

void ConnectionAcceptor::SetClientSocketOptions(int socket_fd) const {
  if (config_.recv_buffer_size > 0 &&
      setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &config_.recv_buffer_size, sizeof(config_.recv_buffer_size)) < 0) {
    spdlog::warn("Failed to set SO_RCVBUF on accepted socket: {}", strerror(errno));
  }
  if (config_.send_buffer_size > 0 &&
      setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &config_.send_buffer_size, sizeof(config_.send_buffer_size)) < 0) {
    spdlog::warn("Failed to set SO_SNDBUF on accepted socket: {}", strerror(errno));
  }
#ifdef __APPLE__
  const int enabled = 1;
  (void)setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

void ConnectionAcceptor::RemoveConnection(int socket_fd) {
  std::lock_guard<std::mutex> lock(fds_mutex_);
  active_fds_.erase(socket_fd);

  // Decrement per-IP connection count
  auto ip_it = fd_to_ip_.find(socket_fd);
  if (ip_it != fd_to_ip_.end()) {
    auto& count = per_ip_connections_[ip_it->second];
    if (count > 0) {
      --count;
    }
    if (count == 0) {
      per_ip_connections_.erase(ip_it->second);
    }
    fd_to_ip_.erase(ip_it);
  }
}

}  // namespace nvecd::server

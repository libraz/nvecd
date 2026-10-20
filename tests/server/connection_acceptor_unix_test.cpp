/**
 * @file connection_acceptor_unix_test.cpp
 * @brief Unit tests for Unix domain socket functionality in ConnectionAcceptor and NvecdServer
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config/config.h"
#include "server/connection_acceptor.h"
#include "server/connection_budget.h"
#include "server/nvecd_server.h"
#include "server/server_types.h"
#include "server/thread_pool.h"
#include "utils/network_utils.h"

namespace fs = std::filesystem;

// ============================================================================
// Test fixture
// ============================================================================

class ConnectionAcceptorUnixTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_directory_ = fs::temp_directory_path() /
                      ("nvecd_uds_test_" + std::to_string(getpid()) + "_" + std::to_string(test_counter_++));
    fs::create_directories(test_directory_);
    fs::permissions(test_directory_, fs::perms::owner_all, fs::perm_options::replace);
    socket_path_ = (test_directory_ / "server.sock").string();
  }

  void TearDown() override { fs::remove_all(test_directory_); }

  nvecd::server::ServerConfig MakeUdsConfig() {
    nvecd::server::ServerConfig config;
    config.unix_socket_path = socket_path_;
    config.max_connections = 10;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
    return config;
  }

  nvecd::server::ServerConfig MakeTcpConfig() {
    nvecd::server::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.max_connections = 10;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
    return config;
  }

  /// Helper: connect to UDS and send a command, returning the response
  std::string SendUdsCommand(const std::string& path, const std::string& command) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
      return "";
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                sizeof(addr)) < 0) {
      close(sock);
      return "";
    }

    // Set recv timeout
    struct timeval tv {};
    tv.tv_sec = 5;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string request = command + "\r\n";
    send(sock, request.c_str(), request.length(), 0);

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
    char buffer[4096];
    ssize_t received = recv(sock, buffer, sizeof(buffer) - 1, 0);
    close(sock);

    if (received <= 0) {
      return "";
    }
    buffer[received] = '\0';
    return std::string(buffer);
  }

  int ConnectUds(const std::string& path) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                sizeof(addr)) < 0) {
      close(sock);
      return -1;
    }
    return sock;
  }

  int ConnectTcp(uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                sizeof(addr)) < 0) {
      close(sock);
      return -1;
    }
    return sock;
  }

  /// True when the server closes the connection without sending anything.
  static bool ClosedByServer(int sock) {
    pollfd poll_fd{sock, POLLIN, 0};
    if (poll(&poll_fd, 1, 2000) <= 0) {  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
      return false;
    }
    char byte = 0;
    return recv(sock, &byte, 1, 0) == 0;
  }

  /// Send INFO and wait for any reply, proving the server registered the connection.
  static bool Roundtrip(int sock) {
    const std::string request = "INFO\r\n";
    if (send(sock, request.c_str(), request.length(), 0) < 0) {
      return false;
    }
    pollfd poll_fd{sock, POLLIN, 0};
    if (poll(&poll_fd, 1, 2000) <= 0) {  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
      return false;
    }
    char buffer[64];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    return recv(sock, buffer, sizeof(buffer), 0) > 0;
  }

  std::string socket_path_;
  fs::path test_directory_;
  static int test_counter_;
};

int ConnectionAcceptorUnixTest::test_counter_ = 0;

// ============================================================================
// Test 1: StartAndStopUnixSocket
// ============================================================================

TEST_F(ConnectionAcceptorUnixTest, StartAndStopUnixSocket) {
  nvecd::server::ThreadPool pool(2);
  auto config = MakeUdsConfig();
  nvecd::server::ConnectionAcceptor acceptor(config, &pool);

  acceptor.SetConnectionHandler([](int) {});

  auto result = acceptor.Start();
  ASSERT_TRUE(result.has_value()) << "Start failed: " << result.error().message();

  EXPECT_TRUE(acceptor.IsRunning());
  EXPECT_TRUE(acceptor.IsUnixSocket());
  EXPECT_TRUE(fs::exists(socket_path_));

  acceptor.Stop();

  EXPECT_FALSE(acceptor.IsRunning());
  EXPECT_FALSE(fs::exists(socket_path_));
}

// ============================================================================
// Test 2: IsUnixSocket_True
// ============================================================================

TEST_F(ConnectionAcceptorUnixTest, IsUnixSocket_True) {
  nvecd::server::ThreadPool pool(2);
  auto config = MakeUdsConfig();
  nvecd::server::ConnectionAcceptor acceptor(config, &pool);

  acceptor.SetConnectionHandler([](int) {});

  auto result = acceptor.Start();
  ASSERT_TRUE(result.has_value()) << "Start failed: " << result.error().message();

  EXPECT_TRUE(acceptor.IsUnixSocket());

  acceptor.Stop();
}

// ============================================================================
// Test 3: IsUnixSocket_False
// ============================================================================

TEST_F(ConnectionAcceptorUnixTest, IsUnixSocket_False) {
  nvecd::server::ThreadPool pool(2);
  auto config = MakeTcpConfig();
  nvecd::server::ConnectionAcceptor acceptor(config, &pool);

  acceptor.SetConnectionHandler([](int) {});

  auto result = acceptor.Start();
  ASSERT_TRUE(result.has_value()) << "Start failed: " << result.error().message();

  EXPECT_FALSE(acceptor.IsUnixSocket());

  acceptor.Stop();
}

// ============================================================================
// Test 4: StaleSocketCleanup
// ============================================================================

TEST_F(ConnectionAcceptorUnixTest, RegularFileAtSocketPathIsPreservedAndRejected) {
  {
    std::ofstream ofs(socket_path_);
    ASSERT_TRUE(ofs.good()) << "Failed to create stale file at " << socket_path_;
  }
  ASSERT_TRUE(fs::exists(socket_path_));

  nvecd::server::ThreadPool pool(2);
  auto config = MakeUdsConfig();
  nvecd::server::ConnectionAcceptor acceptor(config, &pool);

  acceptor.SetConnectionHandler([](int) {});

  auto result = acceptor.Start();
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(fs::exists(socket_path_));
  EXPECT_TRUE(fs::is_regular_file(socket_path_));
}

TEST_F(ConnectionAcceptorUnixTest, OwnedStaleSocketIsRemovedByIdentity) {
  const int stale_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(stale_fd, 0);
  struct sockaddr_un stale_address {};
  stale_address.sun_family = AF_UNIX;
  std::strncpy(stale_address.sun_path, socket_path_.c_str(), sizeof(stale_address.sun_path) - 1);
  ASSERT_EQ(::bind(stale_fd, reinterpret_cast<sockaddr*>(&stale_address), sizeof(stale_address)), 0);
  ::close(stale_fd);

  nvecd::server::ThreadPool pool(2);
  auto config = MakeUdsConfig();
  nvecd::server::ConnectionAcceptor acceptor(config, &pool);
  acceptor.SetConnectionHandler([](int) {});

  auto result = acceptor.Start();
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_TRUE(acceptor.IsRunning());
  EXPECT_TRUE(fs::is_socket(socket_path_));

  acceptor.Stop();
}

TEST_F(ConnectionAcceptorUnixTest, StopPreservesReplacementAtSocketPath) {
  nvecd::server::ThreadPool pool(2);
  auto config = MakeUdsConfig();
  nvecd::server::ConnectionAcceptor acceptor(config, &pool);
  acceptor.SetConnectionHandler([](int) {});
  ASSERT_TRUE(acceptor.Start());

  ASSERT_EQ(::unlink(socket_path_.c_str()), 0);
  {
    std::ofstream replacement(socket_path_);
    ASSERT_TRUE(replacement.good());
    replacement << "preserve me";
  }
  acceptor.Stop();

  EXPECT_TRUE(fs::is_regular_file(socket_path_));
}

// ============================================================================
// Test 5: PathTooLongError
// ============================================================================

TEST_F(ConnectionAcceptorUnixTest, PathTooLongError) {
  nvecd::server::ThreadPool pool(2);

  // Create a path longer than sockaddr_un.sun_path (typically 104 on macOS, 108 on Linux)
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
  std::string long_path = "/tmp/" + std::string(200, 'x') + ".sock";

  nvecd::server::ServerConfig config;
  config.unix_socket_path = long_path;
  config.max_connections = 10;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)

  nvecd::server::ConnectionAcceptor acceptor(config, &pool);
  acceptor.SetConnectionHandler([](int) {});

  auto result = acceptor.Start();
  EXPECT_FALSE(result.has_value()) << "Start should fail for path exceeding sun_path limit";
}

// ============================================================================
// Test 6: SocketFileRemovedOnStop
// ============================================================================

TEST_F(ConnectionAcceptorUnixTest, SocketFileRemovedOnStop) {
  nvecd::server::ThreadPool pool(2);
  auto config = MakeUdsConfig();
  nvecd::server::ConnectionAcceptor acceptor(config, &pool);

  acceptor.SetConnectionHandler([](int) {});

  auto result = acceptor.Start();
  ASSERT_TRUE(result.has_value()) << "Start failed: " << result.error().message();

  // Verify socket file exists while running
  ASSERT_TRUE(fs::exists(socket_path_));

  acceptor.Stop();

  // Verify socket file is removed after stop
  EXPECT_FALSE(fs::exists(socket_path_));
}

// ============================================================================
// Test 7: AcceptsUnixConnection
// ============================================================================

TEST_F(ConnectionAcceptorUnixTest, AcceptsUnixConnection) {
  nvecd::config::Config config;
  config.api.tcp.bind = "127.0.0.1";
  config.api.tcp.port = 0;
  config.api.unix_socket.path = socket_path_;
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.perf.max_connections = 10;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
  config.perf.thread_pool_size = 2;
  config.vectors.default_dimension = 3;
  config.events.ctx_buffer_size = 100;   // NOLINT(cppcoreguidelines-avoid-magic-numbers)
  config.similarity.default_top_k = 10;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
  config.similarity.max_top_k = 100;     // NOLINT(cppcoreguidelines-avoid-magic-numbers)

  auto server = std::make_unique<nvecd::server::NvecdServer>(config);
  ASSERT_TRUE(server->Start().has_value());

  // Allow server to stabilize
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Send INFO command via Unix domain socket
  std::string response = SendUdsCommand(socket_path_, "INFO");
  EXPECT_FALSE(response.empty()) << "Expected non-empty response from UDS connection";
  EXPECT_NE(response.find("OK INFO"), std::string::npos) << "Response should contain 'OK INFO', got: " << response;

  server->Stop();
}

TEST_F(ConnectionAcceptorUnixTest, UnixConnectionsDoNotUseBogusIpRateLimitKey) {
  nvecd::config::Config config;
  config.api.tcp.port = 0;
  config.api.unix_socket.path = socket_path_;
  config.perf.max_connections = 10;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
  config.perf.thread_pool_size = 2;
  config.vectors.default_dimension = 3;
  config.events.ctx_buffer_size = 100;   // NOLINT(cppcoreguidelines-avoid-magic-numbers)
  config.similarity.default_top_k = 10;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
  config.similarity.max_top_k = 100;     // NOLINT(cppcoreguidelines-avoid-magic-numbers)
  config.api.rate_limiting.enable = true;
  config.api.rate_limiting.capacity = 1;
  config.api.rate_limiting.refill_rate = 1;
  config.api.rate_limiting.max_clients = 1;

  auto server = std::make_unique<nvecd::server::NvecdServer>(config);
  ASSERT_TRUE(server->Start().has_value());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // UDS authorization is governed by the socket file permissions. It must not
  // be reinterpreted as a made-up IPv4 identity that exhausts a shared bucket.
  EXPECT_NE(SendUdsCommand(socket_path_, "INFO").find("OK INFO"), std::string::npos);
  EXPECT_NE(SendUdsCommand(socket_path_, "INFO").find("OK INFO"), std::string::npos);

  server->Stop();
}

// ============================================================================
// Test 8: SocketPermissions
// ============================================================================

TEST_F(ConnectionAcceptorUnixTest, SocketPermissions) {
  nvecd::server::ThreadPool pool(2);
  auto config = MakeUdsConfig();
  nvecd::server::ConnectionAcceptor acceptor(config, &pool);

  acceptor.SetConnectionHandler([](int) {});

  auto result = acceptor.Start();
  ASSERT_TRUE(result.has_value()) << "Start failed: " << result.error().message();

  struct stat st {};
  ASSERT_EQ(stat(socket_path_.c_str(), &st), 0) << "Failed to stat socket file";

  // Check permission bits (mask with 0777 to get permission bits only)
  // Socket should be accessible by owner and group only (0770)
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
  EXPECT_EQ(st.st_mode & 0777, 0770) << "Socket permissions should be 0770, got: " << std::oct << (st.st_mode & 0777);

  acceptor.Stop();
}

// ============================================================================
// Test 9: Shared connection budget across listeners
// ============================================================================

TEST_F(ConnectionAcceptorUnixTest, SharedBudgetGatesAdmissionAndIsReturnedOnClose) {
  nvecd::server::ThreadPool pool(2);
  auto config = MakeUdsConfig();
  nvecd::server::ConnectionAcceptor acceptor(config, &pool);

  std::atomic<int> handled{0};
  acceptor.SetConnectionHandler([&handled](int) { ++handled; });
  // Another listener already holds the whole budget.
  auto budget = std::make_shared<nvecd::server::ConnectionBudget>(2);
  ASSERT_TRUE(budget->TryAcquire());
  ASSERT_TRUE(budget->TryAcquire());
  acceptor.SetConnectionBudget(budget);
  ASSERT_TRUE(acceptor.Start().has_value());

  int refused = ConnectUds(socket_path_);
  ASSERT_GE(refused, 0);
  EXPECT_TRUE(ClosedByServer(refused));
  close(refused);
  EXPECT_EQ(handled.load(), 0);

  budget->Release();
  int admitted = ConnectUds(socket_path_);
  ASSERT_GE(admitted, 0);
  auto wait_until = [](auto&& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };
  wait_until([&] { return handled.load() == 1; });
  close(admitted);
  EXPECT_EQ(handled.load(), 1);
  // The handler returned, so the slot it took is back; only the one held above remains.
  wait_until([&] { return budget->Used() == 1; });
  EXPECT_EQ(budget->Used(), 1U);

  acceptor.Stop();
}

TEST_F(ConnectionAcceptorUnixTest, RacingAcceptorsNeverExceedTheSharedBudget) {
  constexpr size_t kMax = 4;
  constexpr int kClients = 24;
  nvecd::server::ThreadPool pool(8);
  auto budget = std::make_shared<nvecd::server::ConnectionBudget>(kMax);

  std::atomic<int> current{0};
  std::atomic<int> peak{0};
  std::atomic<int> handled{0};
  std::atomic<bool> release{false};
  auto handler = [&](int) {
    const int now = ++current;
    int seen = peak.load();
    while (now > seen && !peak.compare_exchange_weak(seen, now)) {
    }
    ++handled;
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    --current;
  };

  auto tcp_config = MakeTcpConfig();
  tcp_config.allow_cidrs = {"127.0.0.1/32"};
  tcp_config.parsed_allow_cidrs = nvecd::utils::ParseCidrList(tcp_config.allow_cidrs);
  nvecd::server::ConnectionAcceptor tcp(tcp_config, &pool);
  nvecd::server::ConnectionAcceptor uds(MakeUdsConfig(), &pool);
  tcp.SetConnectionBudget(budget);
  uds.SetConnectionBudget(budget);
  tcp.SetConnectionHandler(handler);
  uds.SetConnectionHandler(handler);
  ASSERT_TRUE(tcp.Start().has_value());
  ASSERT_TRUE(uds.Start().has_value());

  std::vector<int> sockets(kClients, -1);
  std::vector<std::thread> clients;
  for (int index = 0; index < kClients; ++index) {
    clients.emplace_back(
        [&, index] { sockets[index] = index % 2 == 0 ? ConnectTcp(tcp.GetPort()) : ConnectUds(socket_path_); });
  }
  for (auto& client : clients) {
    client.join();
  }
  // A SYN dropped on a full listen queue is retried after a second, so wait for the budget to fill.
  const auto fill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (handled.load() < static_cast<int>(kMax) && std::chrono::steady_clock::now() < fill_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  EXPECT_EQ(handled.load(), static_cast<int>(kMax));
  EXPECT_LE(peak.load(), static_cast<int>(kMax));
  EXPECT_EQ(budget->Used(), kMax);

  release.store(true);
  for (int sock : sockets) {
    if (sock >= 0) {
      close(sock);
    }
  }
  // Worker tasks reference the acceptors and the locals above; drain them first.
  pool.Shutdown();
  tcp.Stop();
  uds.Stop();
}

TEST_F(ConnectionAcceptorUnixTest, ServerSharesMaxConnectionsBetweenTcpAndUnixListeners) {
  nvecd::config::Config config;
  config.api.tcp.port = 0;
  config.api.unix_socket.path = socket_path_;
  config.perf.max_connections = 2;
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.perf.thread_pool_size = 2;
  config.vectors.default_dimension = 3;
  config.events.ctx_buffer_size = 100;   // NOLINT(cppcoreguidelines-avoid-magic-numbers)
  config.similarity.default_top_k = 10;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
  config.similarity.max_top_k = 100;     // NOLINT(cppcoreguidelines-avoid-magic-numbers)

  auto server = std::make_unique<nvecd::server::NvecdServer>(config);
  ASSERT_TRUE(server->Start().has_value());

  int tcp = ConnectTcp(server->GetPort());
  ASSERT_GE(tcp, 0);
  ASSERT_TRUE(Roundtrip(tcp));
  int uds = ConnectUds(socket_path_);
  ASSERT_GE(uds, 0);
  ASSERT_TRUE(Roundtrip(uds));

  // Two connections fill the budget, so neither listener admits a third.
  int extra_uds = ConnectUds(socket_path_);
  ASSERT_GE(extra_uds, 0);
  EXPECT_TRUE(ClosedByServer(extra_uds));
  int extra_tcp = ConnectTcp(server->GetPort());
  ASSERT_GE(extra_tcp, 0);
  EXPECT_TRUE(ClosedByServer(extra_tcp));

  close(extra_uds);
  close(extra_tcp);
  close(uds);
  close(tcp);
  server->Stop();
}

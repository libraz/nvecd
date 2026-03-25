/**
 * @file connection_acceptor_unix_test.cpp
 * @brief Unit tests for Unix domain socket functionality in ConnectionAcceptor and NvecdServer
 */

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "config/config.h"
#include "server/connection_acceptor.h"
#include "server/nvecd_server.h"
#include "server/server_types.h"
#include "server/thread_pool.h"

namespace fs = std::filesystem;

// ============================================================================
// Test fixture
// ============================================================================

class ConnectionAcceptorUnixTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Generate unique socket path per test
    socket_path_ = "/tmp/nvecd_test_" + std::to_string(getpid()) + "_" + std::to_string(test_counter_++) + ".sock";
    // Ensure no leftover
    unlink(socket_path_.c_str());
  }

  void TearDown() override { unlink(socket_path_.c_str()); }

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

  std::string socket_path_;
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

  acceptor.SetConnectionHandler([](int client_fd) { close(client_fd); });

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

  acceptor.SetConnectionHandler([](int client_fd) { close(client_fd); });

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

  acceptor.SetConnectionHandler([](int client_fd) { close(client_fd); });

  auto result = acceptor.Start();
  ASSERT_TRUE(result.has_value()) << "Start failed: " << result.error().message();

  EXPECT_FALSE(acceptor.IsUnixSocket());

  acceptor.Stop();
}

// ============================================================================
// Test 4: StaleSocketCleanup
// ============================================================================

TEST_F(ConnectionAcceptorUnixTest, StaleSocketCleanup) {
  // Create a dummy file at the socket path (simulating a stale socket)
  {
    std::ofstream ofs(socket_path_);
    ASSERT_TRUE(ofs.good()) << "Failed to create stale file at " << socket_path_;
  }
  ASSERT_TRUE(fs::exists(socket_path_));

  nvecd::server::ThreadPool pool(2);
  auto config = MakeUdsConfig();
  nvecd::server::ConnectionAcceptor acceptor(config, &pool);

  acceptor.SetConnectionHandler([](int client_fd) { close(client_fd); });

  // Start should succeed by removing the stale file and creating a new socket
  auto result = acceptor.Start();
  ASSERT_TRUE(result.has_value()) << "Start failed with stale socket: " << result.error().message();

  EXPECT_TRUE(acceptor.IsRunning());
  EXPECT_TRUE(fs::exists(socket_path_));

  acceptor.Stop();
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
  acceptor.SetConnectionHandler([](int client_fd) { close(client_fd); });

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

  acceptor.SetConnectionHandler([](int client_fd) { close(client_fd); });

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

// ============================================================================
// Test 8: SocketPermissions
// ============================================================================

TEST_F(ConnectionAcceptorUnixTest, SocketPermissions) {
  nvecd::server::ThreadPool pool(2);
  auto config = MakeUdsConfig();
  nvecd::server::ConnectionAcceptor acceptor(config, &pool);

  acceptor.SetConnectionHandler([](int client_fd) { close(client_fd); });

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

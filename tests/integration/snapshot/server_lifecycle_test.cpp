/**
 * @file server_lifecycle_test.cpp
 * @brief NvecdServer start-failure teardown and graceful shutdown drain
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "config/config.h"
#include "server/nvecd_server.h"
#include "test_tcp_client.h"

namespace {

namespace fs = std::filesystem;

/// Listening socket on an ephemeral loopback port, so a second bind fails.
class OccupiedPort {
 public:
  OccupiedPort() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    socklen_t length = sizeof(address);
    if (fd_ < 0 || ::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(fd_, 1) != 0 ||
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      return;
    }
    port_ = ntohs(address.sin_port);
  }
  ~OccupiedPort() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  OccupiedPort(const OccupiedPort&) = delete;
  OccupiedPort& operator=(const OccupiedPort&) = delete;

  [[nodiscard]] uint16_t port() const { return port_; }

 private:
  int fd_ = -1;
  uint16_t port_ = 0;
};

}  // namespace

TEST(StartFailureCleanup, FailedStartStopsTheSchedulersAndClosesTheWal) {
  const auto root = fs::temp_directory_path() / ("nvecd_start_failure_" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "snapshots");
  fs::create_directories(root / "wal");

  OccupiedPort occupied;
  ASSERT_NE(occupied.port(), 0);

  nvecd::config::Config config;
  config.api.tcp.bind = "127.0.0.1";
  config.api.tcp.port = occupied.port();
  config.api.http.enable = false;
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.perf.thread_pool_size = 2;
  config.events.decay_interval_sec = 1;
  config.snapshot.dir = (root / "snapshots").string();
  config.snapshot.interval_sec = 1;
  config.wal.enabled = true;
  config.wal.dir = (root / "wal").string();

  {
    nvecd::server::NvecdServer server(config);
    ASSERT_FALSE(server.Start().has_value());

    // A scheduler left running would fork its first auto snapshot within a
    // second of startup.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    for (const auto& entry : fs::directory_iterator(root / "snapshots")) {
      EXPECT_NE(entry.path().filename().string().rfind("auto_", 0), 0U) << entry.path();
    }
  }

  // The WAL was closed cleanly: a fresh server can open and replay it.
  config.api.tcp.port = 0;
  nvecd::server::NvecdServer recovered(config);
  auto started = recovered.Start();
  EXPECT_TRUE(started.has_value()) << started.error().message();
  recovered.Stop();
  fs::remove_all(root);
}

TEST(GracefulShutdown, EveryExecutedRequestIsAnsweredBeforeConnectionsClose) {
  const auto root = fs::temp_directory_path() / ("nvecd_shutdown_drain_" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "snapshots");
  fs::create_directories(root / "wal");

  nvecd::config::Config config;
  config.api.tcp.bind = "127.0.0.1";
  config.api.tcp.port = 0;
  config.api.http.enable = false;
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.perf.thread_pool_size = 2;
  config.perf.shutdown_timeout_ms = 10000;
  config.events.decay_interval_sec = 0;
  config.snapshot.dir = (root / "snapshots").string();
  config.wal.enabled = true;
  config.wal.dir = (root / "wal").string();
  config.wal.sync_on_write = true;

  constexpr int kCommands = 300;
  size_t answered = 0;
  {
    nvecd::server::NvecdServer server(config);
    ASSERT_TRUE(server.Start());
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server.GetPort());
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    ASSERT_EQ(::connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);

    std::string batch;
    for (int index = 0; index < kCommands; ++index) {
      batch += "VECSET drain-" + std::to_string(index) + " 1 0\r\n";
    }
    ASSERT_EQ(::send(sock, batch.data(), batch.size(), 0), static_cast<ssize_t>(batch.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::thread stopper([&server] { server.Stop(); });

    std::string received;
    char buffer[4096];
    ssize_t count = 0;
    while ((count = ::recv(sock, buffer, sizeof(buffer), 0)) > 0) {
      received.append(buffer, static_cast<size_t>(count));
    }
    stopper.join();
    ::close(sock);
    for (size_t pos = received.find("OK"); pos != std::string::npos; pos = received.find("OK", pos + 2)) {
      ++answered;
    }
  }

  // Every acknowledged write is in the WAL; an executed write whose response
  // was dropped would show up as a recovered vector nobody was told about.
  nvecd::server::NvecdServer recovered(config);
  ASSERT_TRUE(recovered.Start());
  {
    TcpClient client("127.0.0.1", recovered.GetPort());
    const std::string info = client.SendCommand("INFO");
    const auto pos = info.find("vector_count: ");
    ASSERT_NE(pos, std::string::npos) << info;
    EXPECT_EQ(std::stoul(info.substr(pos + 14)), answered);
  }
  recovered.Stop();
  fs::remove_all(root);
}

/**
 * @file reactor_integration_test.cpp
 * @brief Ensures persistent TCP clients do not pin reactor worker threads.
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "config/config.h"
#include "server/nvecd_server.h"

namespace nvecd::server {
namespace {

int Connect(uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
      ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

TEST(ReactorIntegrationTest, PersistentIdleClientsDoNotStarveLateCommand) {
  config::Config config;
  config.api.tcp.bind = "127.0.0.1";
  config.api.tcp.port = 0;
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.perf.thread_pool_size = 1;
  config.perf.max_connections = 64;
  config.perf.connection_timeout_sec = 10;
  config.vectors.default_dimension = 3;

  NvecdServer server(config);
  ASSERT_TRUE(server.Start());

  std::vector<int> clients;
  constexpr int kIdleClients = 24;
  for (int i = 0; i < kIdleClients; ++i) {
    const int fd = Connect(server.GetPort());
    ASSERT_GE(fd, 0);
    clients.push_back(fd);
  }

  const auto accepted_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (server.GetConnectionCount() < static_cast<size_t>(kIdleClients) &&
         std::chrono::steady_clock::now() < accepted_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(server.GetConnectionCount(), static_cast<size_t>(kIdleClients));

  const int late_client = Connect(server.GetPort());
  ASSERT_GE(late_client, 0);
  clients.push_back(late_client);
  constexpr char kRequest[] = "INFO\r\n";
  ASSERT_EQ(::send(late_client, kRequest, sizeof(kRequest) - 1, 0), static_cast<ssize_t>(sizeof(kRequest) - 1));

  pollfd event{late_client, POLLIN, 0};
  ASSERT_GT(::poll(&event, 1, 500), 0) << "late command was starved by idle clients";
  char response[256]{};
  const ssize_t received = ::recv(late_client, response, sizeof(response), 0);
  ASSERT_GT(received, 0);
  EXPECT_EQ(std::string(response, static_cast<size_t>(received)).find("OK"), 0U);

  for (int fd : clients)
    ::close(fd);
  server.Stop();
}

}  // namespace nvecd::server

/**
 * @file tcp_rate_limit_test.cpp
 * @brief Rate limiting on the TCP surface of a real NvecdServer
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

#include "config/config.h"
#include "server/nvecd_server.h"

namespace {

/// One request/response exchange over a fresh loopback connection.
std::string SendTcpCommand(uint16_t port, const std::string& command) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return "";
  }
  const std::string request = command + "\r\n";
  ::send(fd, request.data(), request.size(), 0);
  char buffer[4096];
  const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
  ::close(fd);
  return received > 0 ? std::string(buffer, static_cast<size_t>(received)) : "";
}

}  // namespace

TEST(TcpRateLimit, RequestOverCapacityIsRefusedWithoutSideEffects) {
  nvecd::config::Config config;
  config.api.tcp.bind = "127.0.0.1";
  config.api.tcp.port = 0;
  config.api.http.enable = false;
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.perf.thread_pool_size = 2;
  config.events.decay_interval_sec = 0;
  config.api.rate_limiting.enable = true;
  config.api.rate_limiting.capacity = 1;
  config.api.rate_limiting.refill_rate = 1;
  config.api.rate_limiting.max_clients = 10;

  nvecd::server::NvecdServer server(config);
  ASSERT_TRUE(server.Start());
  const uint16_t port = server.GetPort();

  EXPECT_EQ(SendTcpCommand(port, "VECSET admitted 1 0").rfind("OK", 0), 0U);
  const std::string refused = SendTcpCommand(port, "VECSET refused 0 1");
  EXPECT_NE(refused.find("ERROR Rate limit exceeded"), std::string::npos) << refused;

  // One token refills per second; spend it on reading back the store.
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  const std::string info = SendTcpCommand(port, "INFO");
  const auto pos = info.find("vector_count: ");
  ASSERT_NE(pos, std::string::npos) << info;
  EXPECT_EQ(std::stoul(info.substr(pos + 14)), 1U);

  server.Stop();
}

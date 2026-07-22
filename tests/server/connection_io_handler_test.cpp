/**
 * @file connection_io_handler_test.cpp
 * @brief Boundary tests for TCP connection request framing
 */

#include "server/connection_io_handler.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <thread>

#include "server/server_types.h"

namespace nvecd::server {

TEST(ConnectionIOHandlerTest, RejectsCompleteRequestOverConfiguredLimit) {
  int sockets[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

  std::atomic<bool> shutdown{false};
  std::atomic<int> processor_calls{0};
  IOConfig config;
  config.recv_buffer_size = 32;
  config.max_query_length = 8;
  config.max_accumulated_bytes = 16;
  config.recv_timeout_sec = 0;
  ConnectionIOHandler handler(
      config,
      [&processor_calls](const std::string&, ConnectionContext&) {
        ++processor_calls;
        return std::string("OK");
      },
      shutdown);

  ConnectionContext context;
  std::thread server_thread([&] { handler.HandleConnection(sockets[0], context); });

  const std::string oversized = "123456789\n";
  ASSERT_EQ(::write(sockets[1], oversized.data(), oversized.size()), static_cast<ssize_t>(oversized.size()));

  char response[128] = {};
  const ssize_t received = ::read(sockets[1], response, sizeof(response));
  ASSERT_GT(received, 0);
  EXPECT_NE(std::string(response, static_cast<size_t>(received)).find("ERROR Request too large"), std::string::npos);
  EXPECT_EQ(processor_calls.load(), 0);

  ::close(sockets[1]);
  server_thread.join();
  ::close(sockets[0]);
}

TEST(ConnectionIOHandlerTest, NormalizesAlreadyTerminatedAuthResponseExactlyOnce) {
  int sockets[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

  std::atomic<bool> shutdown{false};
  IOConfig config;
  config.recv_buffer_size = 32;
  config.max_query_length = 32;
  config.max_accumulated_bytes = 64;
  ConnectionIOHandler handler(
      config, [](const std::string&, ConnectionContext&) { return std::string("+OK\r\n"); }, shutdown);

  ConnectionContext context;
  std::thread server_thread([&] { handler.HandleConnection(sockets[0], context); });
  ASSERT_EQ(::write(sockets[1], "AUTH secret\n", 12), 12);

  char response[32] = {};
  const ssize_t received = ::read(sockets[1], response, sizeof(response));
  ASSERT_GT(received, 0);
  EXPECT_EQ(std::string(response, static_cast<size_t>(received)), "+OK\r\n");

  ::close(sockets[1]);
  server_thread.join();
  ::close(sockets[0]);
}

}  // namespace nvecd::server

/**
 * @file io_reactor_test.cpp
 * @brief Regression tests for the non-blocking TCP reactor.
 */

#include "server/io_reactor.h"

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>

#include "server/reactor_connection.h"
#include "server/thread_pool.h"

namespace nvecd::server {
namespace {

int MakeSocketPair(int (&fds)[2]) {
  fds[0] = -1;
  fds[1] = -1;
  return ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
}

std::string ReadAvailable(int fd, int timeout_ms = 1000) {
  pollfd poll_fd{fd, POLLIN, 0};
  if (::poll(&poll_fd, 1, timeout_ms) <= 0)
    return {};
  char buffer[256]{};
  const ssize_t count = ::read(fd, buffer, sizeof(buffer));
  return count > 0 ? std::string(buffer, static_cast<size_t>(count)) : std::string{};
}

IOConfig TestIoConfig() {
  IOConfig config;
  config.recv_buffer_size = 64;
  config.max_query_length = 64;
  config.max_accumulated_bytes = 256;
  config.recv_timeout_sec = 0;
  return config;
}

}  // namespace

TEST(ReactorMemoryBudgetTest, EnforcesProcessWideAdmissionLimit) {
  ReactorMemoryBudget budget(10);
  EXPECT_TRUE(budget.TryAcquire(7));
  EXPECT_FALSE(budget.TryAcquire(4));
  EXPECT_EQ(budget.UsedBytes(), 7U);
  budget.Release(5);
  EXPECT_TRUE(budget.TryAcquire(4));
  EXPECT_EQ(budget.UsedBytes(), 6U);
}

TEST(IoReactorTest, FramesPartialAndPipelinedRequestsWithoutBlockingTheLoop) {
  ThreadPool pool(1);
  IoReactor reactor({10, 0});
  ASSERT_TRUE(reactor.Start());

  int sockets[2];
  ASSERT_EQ(MakeSocketPair(sockets), 0);
  auto connection =
      ReactorConnection::Create(sockets[0], &reactor, &pool, TestIoConfig(),
                                [](const std::string& request, ConnectionContext&) { return "OK " + request; });
  ASSERT_TRUE(reactor.Register(connection));

  ASSERT_EQ(::write(sockets[1], "HEL", 3), 3);
  EXPECT_EQ(ReadAvailable(sockets[1], 50), "");
  ASSERT_EQ(::write(sockets[1], "LO\nNEXT\n", 8), 8);

  std::string received;
  for (int attempt = 0; attempt < 3 && received.find("OK HELLO\r\nOK NEXT\r\n") == std::string::npos; ++attempt) {
    received += ReadAvailable(sockets[1]);
  }
  EXPECT_EQ(received, "OK HELLO\r\nOK NEXT\r\n");

  reactor.Stop();
  pool.Shutdown();
  connection.reset();
  ::close(sockets[1]);
}

TEST(IoReactorTest, IdleConnectionsDoNotConsumeWorkersBeforeLateRequest) {
  ThreadPool pool(1);
  IoReactor reactor({10, 0});
  ASSERT_TRUE(reactor.Start());

  std::vector<std::shared_ptr<ReactorConnection>> connections;
  std::vector<int> clients;
  constexpr int kIdleClients = 32;
  for (int i = 0; i < kIdleClients; ++i) {
    int sockets[2];
    ASSERT_EQ(MakeSocketPair(sockets), 0);
    auto connection = ReactorConnection::Create(sockets[0], &reactor, &pool, TestIoConfig(),
                                                [](const std::string&, ConnectionContext&) { return "OK"; });
    ASSERT_TRUE(reactor.Register(connection));
    connections.push_back(std::move(connection));
    clients.push_back(sockets[1]);
  }
  EXPECT_EQ(pool.GetQueueSize(), 0U);

  ASSERT_EQ(::write(clients.back(), "INFO\n", 5), 5);
  EXPECT_EQ(ReadAvailable(clients.back(), 500), "OK\r\n");
  EXPECT_EQ(reactor.ConnectionCount(), static_cast<size_t>(kIdleClients));

  reactor.Stop();
  pool.Shutdown();
  connections.clear();
  for (int fd : clients)
    ::close(fd);
}

TEST(IoReactorTest, InitialReadDeadlineClosesSlowlorisPartialRequest) {
  ThreadPool pool(1);
  ReactorConfig config;
  config.poll_timeout_ms = 10;
  config.idle_timeout_sec = 10;
  config.initial_read_timeout_sec = 1;
  config.reaper_interval_sec = 1;
  IoReactor reactor(config);
  ASSERT_TRUE(reactor.Start());

  int sockets[2];
  ASSERT_EQ(MakeSocketPair(sockets), 0);
  auto connection = ReactorConnection::Create(sockets[0], &reactor, &pool, TestIoConfig(),
                                              [](const std::string&, ConnectionContext&) { return "OK"; });
  ASSERT_TRUE(reactor.Register(connection));
  ASSERT_EQ(::write(sockets[1], "PARTIAL", 7), 7);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (reactor.ConnectionCount() != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_EQ(reactor.ConnectionCount(), 0U);

  reactor.Stop();
  pool.Shutdown();
  connection.reset();
  ::close(sockets[1]);
}

}  // namespace nvecd::server

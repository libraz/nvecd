/**
 * @file io_reactor_test.cpp
 * @brief Regression tests for the non-blocking TCP reactor.
 */

#include "server/io_reactor.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
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

std::string ReadUntilLength(int fd, size_t expected_length, int timeout_ms = 1000) {
  std::string received;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (received.size() < expected_length) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      break;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd poll_fd{fd, POLLIN, 0};
    if (::poll(&poll_fd, 1, static_cast<int>(remaining.count())) <= 0)
      break;
    char buffer[256]{};
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count <= 0)
      break;
    received.append(buffer, static_cast<size_t>(count));
  }
  return received;
}

bool PeerClosed(int fd, int timeout_ms = 1000) {
  pollfd poll_fd{fd, POLLIN, 0};
  if (::poll(&poll_fd, 1, timeout_ms) <= 0)
    return false;
  char byte = 0;
  return ::read(fd, &byte, sizeof(byte)) == 0;
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
  ASSERT_TRUE(reactor.Register(sockets[0], &pool, TestIoConfig(),
                               [](const std::string& request, ConnectionContext&) { return "OK " + request; }));

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
  ::close(sockets[1]);
}

TEST(IoReactorTest, IdleConnectionsDoNotConsumeWorkersBeforeLateRequest) {
  ThreadPool pool(1);
  IoReactor reactor({10, 0});
  ASSERT_TRUE(reactor.Start());

  std::vector<int> clients;
  constexpr int kIdleClients = 32;
  for (int i = 0; i < kIdleClients; ++i) {
    int sockets[2];
    ASSERT_EQ(MakeSocketPair(sockets), 0);
    ASSERT_TRUE(reactor.Register(sockets[0], &pool, TestIoConfig(),
                                 [](const std::string&, ConnectionContext&) { return "OK"; }));
    clients.push_back(sockets[1]);
  }
  EXPECT_EQ(pool.GetQueueSize(), 0U);

  ASSERT_EQ(::write(clients.back(), "INFO\n", 5), 5);
  EXPECT_EQ(ReadAvailable(clients.back(), 500), "OK\r\n");
  EXPECT_EQ(reactor.ConnectionCount(), static_cast<size_t>(kIdleClients));

  reactor.Stop();
  pool.Shutdown();
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
  ASSERT_TRUE(
      reactor.Register(sockets[0], &pool, TestIoConfig(), [](const std::string&, ConnectionContext&) { return "OK"; }));
  ASSERT_EQ(::write(sockets[1], "PARTIAL", 7), 7);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (reactor.ConnectionCount() != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_EQ(reactor.ConnectionCount(), 0U);

  reactor.Stop();
  pool.Shutdown();
  ::close(sockets[1]);
}

TEST(IoReactorTest, MixedLineEndingsFrameOneRequestPerLine) {
  ThreadPool pool(1);
  IoReactor reactor({10, 0});
  ASSERT_TRUE(reactor.Start());

  int sockets[2];
  ASSERT_EQ(MakeSocketPair(sockets), 0);
  std::atomic<bool> saw_terminator{false};
  ASSERT_TRUE(reactor.Register(sockets[0], &pool, TestIoConfig(),
                               [&saw_terminator](const std::string& request, ConnectionContext&) {
                                 if (request.find_first_of("\r\n") != std::string::npos)
                                   saw_terminator = true;
                                 return "OK " + request;
                               }));

  ASSERT_EQ(::write(sockets[1], "PING\nINFO\r\nLAST\n", 16), 16);
  std::string received;
  for (int attempt = 0; attempt < 3 && received.size() < 30; ++attempt) {
    received += ReadAvailable(sockets[1]);
  }
  EXPECT_EQ(received, "OK PING\r\nOK INFO\r\nOK LAST\r\n");
  EXPECT_FALSE(saw_terminator.load());

  reactor.Stop();
  pool.Shutdown();
  ::close(sockets[1]);
}

TEST(IoReactorTest, HalfClosedClientReceivesReplyAfterBlockedProcessorResumes) {
  ThreadPool pool(1);
  ReactorConfig config;
  config.poll_timeout_ms = 10;
  IoReactor reactor(config);
  ASSERT_TRUE(reactor.Start());

  int sockets[2];
  ASSERT_EQ(MakeSocketPair(sockets), 0);
  std::atomic<bool> processor_started{false};
  std::atomic<bool> release_processor{false};
  ASSERT_TRUE(
      reactor.Register(sockets[0], &pool, TestIoConfig(),
                       [&processor_started, &release_processor](const std::string& request, ConnectionContext&) {
                         processor_started.store(true, std::memory_order_release);
                         while (!release_processor.load(std::memory_order_acquire))
                           std::this_thread::yield();
                         return "OK " + request;
                       }));

  ASSERT_EQ(::write(sockets[1], "SLOW\n", 5), 5);
  const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!processor_started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < start_deadline)
    std::this_thread::yield();
  if (!processor_started.load(std::memory_order_acquire)) {
    release_processor.store(true, std::memory_order_release);
    reactor.Stop();
    pool.Shutdown();
    ::close(sockets[1]);
    FAIL() << "request processor did not start";
  }

  const int shutdown_result = ::shutdown(sockets[1], SHUT_WR);
  const auto hangup_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
  while (reactor.ConnectionCount() == 1 && std::chrono::steady_clock::now() < hangup_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool stayed_registered = reactor.ConnectionCount() == 1;

  release_processor.store(true, std::memory_order_release);
  const std::string expected = "OK SLOW\r\n";
  const std::string received = ReadUntilLength(sockets[1], expected.size());
  const bool peer_closed = PeerClosed(sockets[1]);
  EXPECT_EQ(shutdown_result, 0);
  EXPECT_TRUE(stayed_registered) << "read hangup must not discard an in-flight request";
  EXPECT_EQ(received, expected);
  EXPECT_TRUE(peer_closed) << "the connection closes after flushing the response";

  reactor.Stop();
  pool.Shutdown();
  ::close(sockets[1]);
}

double ProcessCpuMilliseconds() {
  rusage usage{};
  ::getrusage(RUSAGE_SELF, &usage);
  const auto to_ms = [](const timeval& value) { return value.tv_sec * 1000.0 + value.tv_usec / 1000.0; };
  return to_ms(usage.ru_utime) + to_ms(usage.ru_stime);
}

TEST(IoReactorTest, FullyClosedClientDoesNotSpinTheReactorWhileItsRequestRuns) {
  ThreadPool pool(1);
  ReactorConfig config;
  config.poll_timeout_ms = 10;
  IoReactor reactor(config);
  ASSERT_TRUE(reactor.Start());

  int sockets[2];
  ASSERT_EQ(MakeSocketPair(sockets), 0);
  std::atomic<bool> processor_started{false};
  ASSERT_TRUE(reactor.Register(sockets[0], &pool, TestIoConfig(),
                               [&processor_started](const std::string& request, ConnectionContext&) {
                                 processor_started.store(true, std::memory_order_release);
                                 std::this_thread::sleep_for(std::chrono::milliseconds(300));
                                 return "OK " + request;
                               }));

  ASSERT_EQ(::write(sockets[1], "SLOW\n", 5), 5);
  const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!processor_started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < start_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(processor_started.load(std::memory_order_acquire));

  ::close(sockets[1]);
  const double cpu_before = ProcessCpuMilliseconds();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const double cpu_spent = ProcessCpuMilliseconds() - cpu_before;
  EXPECT_LT(cpu_spent, 100.0) << "a peer hangup must not be reported to the reactor on every poll";

  reactor.Stop();
  pool.Shutdown();
}

TEST(IoReactorTest, ProcessorExceptionReturnsErrorAndLeavesDrainAvailable) {
  ThreadPool pool(1);
  IoReactor reactor({10, 0});
  ASSERT_TRUE(reactor.Start());

  int sockets[2];
  ASSERT_EQ(MakeSocketPair(sockets), 0);
  ASSERT_TRUE(reactor.Register(sockets[0], &pool, TestIoConfig(), [](const std::string& request, ConnectionContext&) {
    if (request == "THROW")
      throw std::runtime_error("private processor detail");
    return "OK " + request;
  }));

  const std::string pipelined = "THROW\nINFO\n";
  ASSERT_EQ(::write(sockets[1], pipelined.data(), pipelined.size()), static_cast<ssize_t>(pipelined.size()));
  const std::string first_expected = "ERROR Internal server error\r\nOK INFO\r\n";
  EXPECT_EQ(ReadUntilLength(sockets[1], first_expected.size()), first_expected);

  ASSERT_EQ(::write(sockets[1], "NEXT\n", 5), 5);
  EXPECT_EQ(ReadAvailable(sockets[1]), "OK NEXT\r\n");

  reactor.Stop();
  pool.Shutdown();
  ::close(sockets[1]);
}

TEST(IoReactorTest, ServerBusyReplyIsFlushedBeforeTheClose) {
  ThreadPool pool(1);
  pool.Shutdown();  // Every Submit now fails, as with a saturated pool.
  IoReactor reactor({10, 0});
  ASSERT_TRUE(reactor.Start());

  int sockets[2];
  ASSERT_EQ(MakeSocketPair(sockets), 0);
  ASSERT_TRUE(
      reactor.Register(sockets[0], &pool, TestIoConfig(), [](const std::string&, ConnectionContext&) { return "OK"; }));

  ASSERT_EQ(::write(sockets[1], "INFO\n", 5), 5);
  EXPECT_EQ(ReadAvailable(sockets[1]), "ERROR Server busy\r\n");
  EXPECT_EQ(ReadAvailable(sockets[1]), "");  // then end of stream

  reactor.Stop();
  ::close(sockets[1]);
}

TEST(IoReactorTest, ExecutingRequestIsNotReapedAsIdle) {
  ThreadPool pool(1);
  ReactorConfig config;
  config.poll_timeout_ms = 10;
  config.idle_timeout_sec = 1;
  config.reaper_interval_sec = 1;
  IoReactor reactor(config);
  ASSERT_TRUE(reactor.Start());

  int sockets[2];
  ASSERT_EQ(MakeSocketPair(sockets), 0);
  ASSERT_TRUE(reactor.Register(sockets[0], &pool, TestIoConfig(), [](const std::string&, ConnectionContext&) {
    // Runs well past the idle timeout with no socket activity.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    return std::string("OK");
  }));

  ASSERT_EQ(::write(sockets[1], "SLOW\n", 5), 5);
  EXPECT_EQ(ReadAvailable(sockets[1], 5000), "OK\r\n");

  reactor.Stop();
  pool.Shutdown();
  ::close(sockets[1]);
}

TEST(IoReactorTest, ClientThatNeverReadsItsRepliesIsReapedAsIdle) {
  ThreadPool pool(1);
  ReactorConfig config;
  config.poll_timeout_ms = 10;
  config.idle_timeout_sec = 1;
  config.reaper_interval_sec = 1;
  IoReactor reactor(config);
  ASSERT_TRUE(reactor.Start());

  int sockets[2];
  ASSERT_EQ(MakeSocketPair(sockets), 0);
  const std::string big_reply(1024 * 1024, 'x');
  ASSERT_TRUE(reactor.Register(sockets[0], &pool, TestIoConfig(),
                               [&big_reply](const std::string&, ConnectionContext&) { return big_reply; }));

  // More reply bytes than the socket buffers hold, so output stays queued.
  ASSERT_EQ(::write(sockets[1], "A\nB\nC\nD\nE\nF\n", 12), 12);
  std::this_thread::sleep_for(std::chrono::milliseconds(3500));

  // Reading now only drains what was flushed before the close; EOF must follow.
  bool closed = false;
  char buffer[65536];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd poll_fd{sockets[1], POLLIN, 0};
    if (::poll(&poll_fd, 1, 200) <= 0)
      continue;
    const ssize_t count = ::read(sockets[1], buffer, sizeof(buffer));
    if (count <= 0) {
      closed = true;
      break;
    }
  }
  EXPECT_TRUE(closed) << "a client that never reads must not pin its queued output past the idle timeout";

  reactor.Stop();
  pool.Shutdown();
  ::close(sockets[1]);
}

TEST(IoReactorTest, FailedRegistrationLeavesTheDescriptorWithTheCaller) {
  ThreadPool pool(1);
  IoReactor reactor({10, 0});

  int sockets[2];
  ASSERT_EQ(MakeSocketPair(sockets), 0);

  // Registering before Start() fails; the fd must survive so that the accept
  // loop, which is still its owner, can close it exactly once.
  auto before_start =
      reactor.Register(sockets[0], &pool, TestIoConfig(), [](const std::string&, ConnectionContext&) { return "OK"; });
  ASSERT_FALSE(before_start.has_value());
  EXPECT_EQ(::fcntl(sockets[0], F_GETFD), 0) << "failed Register must not close the caller's fd";

  ASSERT_TRUE(reactor.Start());
  auto registered =
      reactor.Register(sockets[0], &pool, TestIoConfig(), [](const std::string&, ConnectionContext&) { return "OK"; });
  ASSERT_TRUE(registered.has_value());

  // A second registration of the same fd fails after the reactor already owns
  // it; the rejection must not close the descriptor the first one owns.
  auto duplicate =
      reactor.Register(sockets[0], &pool, TestIoConfig(), [](const std::string&, ConnectionContext&) { return "OK"; });
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(::fcntl(sockets[0], F_GETFD), 0) << "duplicate Register must not close the registered fd";

  reactor.Stop();
  pool.Shutdown();
  registered->reset();
  EXPECT_EQ(::fcntl(sockets[0], F_GETFD), -1) << "the connection must close the fd when it is destroyed";
  ::close(sockets[1]);
}

}  // namespace nvecd::server

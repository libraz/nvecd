/**
 * @file io_reactor.h
 * @brief Event loop that owns non-blocking client sockets.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "server/reactor/event_multiplexer.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server {

class ReactorConnection;
class ThreadPool;

/** Process-wide admission control for buffered, unprocessed socket bytes. */
class ReactorMemoryBudget {
 public:
  explicit ReactorMemoryBudget(size_t limit_bytes) : limit_bytes_(limit_bytes) {}

  bool TryAcquire(size_t bytes) {
    size_t used = used_bytes_.load(std::memory_order_relaxed);
    while (bytes <= limit_bytes_ - used) {
      if (used_bytes_.compare_exchange_weak(used, used + bytes, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }
  void Release(size_t bytes) { used_bytes_.fetch_sub(bytes, std::memory_order_acq_rel); }
  size_t UsedBytes() const { return used_bytes_.load(std::memory_order_relaxed); }
  size_t LimitBytes() const { return limit_bytes_; }

 private:
  const size_t limit_bytes_;
  std::atomic<size_t> used_bytes_{0};
};

struct ReactorConfig {
  int poll_timeout_ms = 100;
  int idle_timeout_sec = 300;
  int initial_read_timeout_sec = 0;
  int reaper_interval_sec = 5;
  size_t max_total_buffered_bytes = 256 * 1024 * 1024;
};

/** A single level-triggered loop. Command execution remains on ThreadPool. */
class IoReactor {
 public:
  explicit IoReactor(ReactorConfig config = {});
  ~IoReactor();
  IoReactor(const IoReactor&) = delete;
  IoReactor& operator=(const IoReactor&) = delete;

  utils::Expected<void, utils::Error> Start();
  void Stop();
  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  utils::Expected<void, utils::Error> Register(std::shared_ptr<ReactorConnection> connection);
  void Unregister(int fd, const ReactorConnection* expected_owner = nullptr);
  void ArmWrite(int fd);
  void DisarmWrite(int fd);
  void DisarmRead(int fd);
  void SetReadEnabled(int fd, bool enabled);
  void SetCloseCallback(std::function<void(int)> callback);
  size_t ConnectionCount() const;
  std::shared_ptr<ReactorMemoryBudget> MemoryBudget() const { return memory_budget_; }

 private:
  struct Entry {
    std::shared_ptr<ReactorConnection> connection;
    reactor::RegistrationToken token = 0;
    uint8_t interest = reactor::event::kReadable;
  };

  void EventLoop();
  void ReapIdleConnections();
  std::shared_ptr<ReactorConnection> Find(reactor::RegistrationToken token, int fd) const;

  ReactorConfig config_;
  std::atomic<bool> running_{false};
  std::unique_ptr<reactor::EventMultiplexer> multiplexer_;
  std::thread event_loop_;
  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex connections_mutex_;
  std::unordered_map<int, Entry> connections_;
  std::unordered_map<reactor::RegistrationToken, int> tokens_;
  reactor::RegistrationToken next_token_ = 1;
  std::function<void(int)> close_callback_;
  std::shared_ptr<ReactorMemoryBudget> memory_budget_;
  std::chrono::steady_clock::time_point last_reaper_run_{std::chrono::steady_clock::now()};
};

}  // namespace nvecd::server

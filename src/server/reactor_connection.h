/**
 * @file reactor_connection.h
 * @brief Per-client state for the non-blocking TCP reactor.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "server/connection_io_handler.h"
#include "server/server_types.h"

namespace nvecd::server {

class IoReactor;
class ReactorMemoryBudget;
class ThreadPool;

class ReactorConnection : public std::enable_shared_from_this<ReactorConnection> {
 public:
  static std::shared_ptr<ReactorConnection> Create(int fd, IoReactor* reactor, ThreadPool* thread_pool, IOConfig config,
                                                   RequestProcessor processor);
  ~ReactorConnection();
  ReactorConnection(const ReactorConnection&) = delete;
  ReactorConnection& operator=(const ReactorConnection&) = delete;

  int Fd() const { return fd_; }
  bool OnReadable();
  bool OnWritable();
  bool OnError();
  std::chrono::steady_clock::time_point LastActive() const;
  std::chrono::steady_clock::time_point CreatedAt() const;
  bool HasReceivedFrame() const { return received_frame_.load(std::memory_order_acquire); }

 private:
  ReactorConnection(int fd, IoReactor* reactor, ThreadPool* thread_pool, IOConfig config, RequestProcessor processor);
  bool ScheduleDrain();
  void DrainRequests();
  bool EnqueueResponse(const std::string& response);
  bool HasPendingOutput() const;
  bool HasPendingRequests() const;
  void CloseAfterFlush();
  static std::string NormalizeResponse(const std::string& response);

  static constexpr size_t kMaxPendingFrames = 1024;
  static constexpr size_t kPendingFramesHighWatermark = 768;
  static constexpr size_t kPendingFramesLowWatermark = 512;
  static constexpr size_t kMaxWriteQueueBytes = 16 * 1024 * 1024;

  int fd_;
  IoReactor* reactor_;
  ThreadPool* thread_pool_;
  IOConfig config_;
  RequestProcessor processor_;
  ConnectionContext context_;

  mutable std::mutex read_mutex_;
  std::string accumulated_;
  std::deque<std::string> requests_;
  size_t request_bytes_ = 0;
  std::atomic<bool> drain_scheduled_{false};
  std::atomic<bool> read_eof_{false};
  bool read_paused_ = false;  // guarded by read_mutex_
  std::atomic<bool> received_frame_{false};

  mutable std::mutex write_mutex_;
  std::deque<std::string> responses_;
  size_t response_offset_ = 0;
  size_t response_bytes_ = 0;
  std::atomic<bool> close_after_flush_{false};
  std::atomic<bool> closed_{false};
  std::atomic<std::chrono::steady_clock::time_point> last_active_;
  std::atomic<std::chrono::steady_clock::time_point> created_at_;
  std::shared_ptr<ReactorMemoryBudget> memory_budget_;
};

}  // namespace nvecd::server

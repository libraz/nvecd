/** @file reactor_connection.cpp */

#include "server/reactor_connection.h"

#include <arpa/inet.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

#include "server/io_reactor.h"
#include "server/thread_pool.h"

namespace nvecd::server {
namespace {
constexpr size_t kReadChunkBytes = 4096;
constexpr size_t kReadEventByteBudget = 64 * 1024;
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
}  // namespace

std::shared_ptr<ReactorConnection> ReactorConnection::Create(int fd, IoReactor* reactor, ThreadPool* thread_pool,
                                                             IOConfig config, RequestProcessor processor) {
  return std::shared_ptr<ReactorConnection>(
      new ReactorConnection(fd, reactor, thread_pool, config, std::move(processor)));
}

ReactorConnection::ReactorConnection(int fd, IoReactor* reactor, ThreadPool* thread_pool, IOConfig config,
                                     RequestProcessor processor)
    : fd_(fd), reactor_(reactor), thread_pool_(thread_pool), config_(config), processor_(std::move(processor)) {
  if (reactor_ != nullptr)
    memory_budget_ = reactor_->MemoryBudget();
  context_.client_fd = fd;
  sockaddr_storage peer{};
  socklen_t peer_len = sizeof(peer);
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0) {
    if (peer.ss_family == AF_INET) {
      const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&peer);
      char address[INET_ADDRSTRLEN]{};
      if (::inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address)) != nullptr)
        context_.client_ip = address;
    } else if (peer.ss_family == AF_UNIX) {
      context_.client_ip = "unix";
    }
  }
  const auto now = std::chrono::steady_clock::now();
  created_at_.store(now, std::memory_order_relaxed);
  last_active_.store(now, std::memory_order_relaxed);
}

ReactorConnection::~ReactorConnection() {
  if (memory_budget_ != nullptr) {
    std::lock_guard<std::mutex> read_lock(read_mutex_);
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    memory_budget_->Release(accumulated_.size() + request_bytes_ + response_bytes_);
  }
  if (!closed_.exchange(true, std::memory_order_acq_rel) && fd_ >= 0)
    ::close(fd_);
}

std::chrono::steady_clock::time_point ReactorConnection::LastActive() const {
  return last_active_.load(std::memory_order_relaxed);
}

std::chrono::steady_clock::time_point ReactorConnection::CreatedAt() const {
  return created_at_.load(std::memory_order_relaxed);
}

bool ReactorConnection::OnReadable() {
  if (closed_.load(std::memory_order_acquire))
    return false;
  last_active_.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);

  std::array<char, kReadChunkBytes> chunk{};
  bool queued = false;
  bool pause_read = false;
  size_t read_bytes = 0;
  while (!read_eof_.load(std::memory_order_acquire)) {
    const ssize_t read = ::recv(fd_, chunk.data(), chunk.size(), 0);
    if (read > 0) {
      read_bytes += static_cast<size_t>(read);
      bool overflow = false;
      {
        std::lock_guard<std::mutex> lock(read_mutex_);
        if (accumulated_.size() + static_cast<size_t>(read) > config_.max_accumulated_bytes ||
            (memory_budget_ != nullptr && !memory_budget_->TryAcquire(static_cast<size_t>(read)))) {
          overflow = true;
        } else {
          accumulated_.append(chunk.data(), static_cast<size_t>(read));
          while (true) {
            size_t end = accumulated_.find("\r\n");
            size_t delimiter = 2;
            if (end == std::string::npos) {
              end = accumulated_.find('\n');
              delimiter = 1;
            }
            if (end == std::string::npos)
              break;
            if (end > config_.max_query_length || requests_.size() >= kMaxPendingFrames ||
                request_bytes_ + end > config_.max_accumulated_bytes) {
              overflow = true;
              break;
            }
            std::string request = accumulated_.substr(0, end);
            // Framed request bytes move from accumulated_ to requests_; only
            // the delimiter leaves the logical buffered-byte accounting.
            if (memory_budget_ != nullptr)
              memory_budget_->Release(delimiter);
            accumulated_.erase(0, end + delimiter);
            if (!request.empty()) {
              received_frame_.store(true, std::memory_order_release);
              request_bytes_ += request.size();
              requests_.push_back(std::move(request));
              queued = true;
            }
          }
          if (requests_.size() >= kPendingFramesHighWatermark) {
            read_paused_ = true;
            pause_read = true;
          }
        }
      }
      if (overflow) {
        spdlog::warn("Reactor request buffer limit reached on fd {}", fd_);
        (void)EnqueueResponse("ERROR Request too large");
        CloseAfterFlush();
        return true;
      }
      if (pause_read) {
        reactor_->SetReadEnabled(fd_, false);
        break;
      }
      // Level-triggered readiness will report this fd again while the peer
      // still has data. Yield after a bounded turn so one streaming client
      // cannot monopolize the reactor thread.
      if (read_bytes >= kReadEventByteBudget)
        break;
      continue;
    }
    if (read == 0) {
      read_eof_.store(true, std::memory_order_release);
      reactor_->DisarmRead(fd_);
      break;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;
    return false;
  }

  if (queued && !ScheduleDrain())
    return false;
  if (read_eof_.load(std::memory_order_acquire) && !HasPendingRequests() && !drain_scheduled_.load()) {
    if (!HasPendingOutput())
      return false;
    CloseAfterFlush();
  }
  return true;
}

bool ReactorConnection::OnWritable() {
  last_active_.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);
  bool drained = false;
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    while (!responses_.empty()) {
      std::string& response = responses_.front();
      const ssize_t sent =
          ::send(fd_, response.data() + response_offset_, response.size() - response_offset_, kSendFlags);
      if (sent > 0) {
        response_offset_ += static_cast<size_t>(sent);
        response_bytes_ -= static_cast<size_t>(sent);
        if (memory_budget_ != nullptr)
          memory_budget_->Release(static_cast<size_t>(sent));
        if (response_offset_ == response.size()) {
          responses_.pop_front();
          response_offset_ = 0;
        }
        continue;
      }
      if (sent < 0 && errno == EINTR)
        continue;
      if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return true;
      return false;
    }
    drained = true;
  }
  if (drained)
    reactor_->DisarmWrite(fd_);
  return !close_after_flush_.load(std::memory_order_acquire);
}

bool ReactorConnection::OnError() {
  return false;
}

bool ReactorConnection::ScheduleDrain() {
  bool expected = false;
  if (!drain_scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    return true;
  if (thread_pool_ == nullptr || !thread_pool_->Submit([self = shared_from_this()] { self->DrainRequests(); })) {
    drain_scheduled_.store(false, std::memory_order_release);
    (void)EnqueueResponse("ERROR Server busy");
    CloseAfterFlush();
    return false;
  }
  return true;
}

void ReactorConnection::DrainRequests() {
  while (true) {
    std::string request;
    bool resume_read = false;
    {
      std::lock_guard<std::mutex> lock(read_mutex_);
      if (requests_.empty()) {
        drain_scheduled_.store(false, std::memory_order_release);
        break;
      }
      request = std::move(requests_.front());
      request_bytes_ -= request.size();
      if (memory_budget_ != nullptr)
        memory_budget_->Release(request.size());
      requests_.pop_front();
      if (read_paused_ && requests_.size() <= kPendingFramesLowWatermark) {
        read_paused_ = false;
        resume_read = true;
      }
    }
    if (resume_read)
      reactor_->SetReadEnabled(fd_, true);
    if (!EnqueueResponse(processor_(request, context_))) {
      CloseAfterFlush();
      break;
    }
  }
  if (read_eof_.load(std::memory_order_acquire) && !HasPendingRequests()) {
    CloseAfterFlush();
    if (!HasPendingOutput())
      reactor_->Unregister(fd_, this);
  }
}

bool ReactorConnection::EnqueueResponse(const std::string& response) {
  std::string framed = NormalizeResponse(response);
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (response_bytes_ + framed.size() > kMaxWriteQueueBytes ||
        (memory_budget_ != nullptr && !memory_budget_->TryAcquire(framed.size()))) {
      spdlog::warn("Reactor write buffer limit reached on fd {}", fd_);
      return false;
    }
    response_bytes_ += framed.size();
    responses_.push_back(std::move(framed));
  }
  reactor_->ArmWrite(fd_);
  return true;
}

bool ReactorConnection::HasPendingOutput() const {
  std::lock_guard<std::mutex> lock(write_mutex_);
  return !responses_.empty();
}

bool ReactorConnection::HasPendingRequests() const {
  std::lock_guard<std::mutex> lock(read_mutex_);
  return !requests_.empty();
}

void ReactorConnection::CloseAfterFlush() {
  close_after_flush_.store(true, std::memory_order_release);
  reactor_->DisarmRead(fd_);
  reactor_->ArmWrite(fd_);
}

std::string ReactorConnection::NormalizeResponse(const std::string& response) {
  size_t end = response.size();
  while (end > 0 && (response[end - 1] == '\r' || response[end - 1] == '\n'))
    --end;
  std::string normalized;
  normalized.reserve(end + 2);
  for (size_t i = 0; i < end; ++i) {
    if (response[i] == '\r')
      continue;
    if (response[i] == '\n')
      normalized += "\r\n";
    else
      normalized += response[i];
  }
  normalized += "\r\n";
  return normalized;
}

}  // namespace nvecd::server

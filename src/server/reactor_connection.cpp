/** @file reactor_connection.cpp */

#include "server/reactor_connection.h"

#include <arpa/inet.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <utility>

#include "server/io_reactor.h"
#include "server/protocol_framing.h"
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

std::shared_ptr<ReactorConnection> ReactorConnection::Create(utils::FDGuard fd, IoReactor* reactor,
                                                             ThreadPool* thread_pool, IOConfig config,
                                                             RequestProcessor processor) {
  return std::shared_ptr<ReactorConnection>(
      new ReactorConnection(std::move(fd), reactor, thread_pool, config, std::move(processor)));
}

ReactorConnection::ReactorConnection(utils::FDGuard fd, IoReactor* reactor, ThreadPool* thread_pool, IOConfig config,
                                     RequestProcessor processor)
    : fd_(std::move(fd)),
      reactor_(reactor),
      thread_pool_(thread_pool),
      config_(config),
      processor_(std::move(processor)) {
  if (reactor_ != nullptr)
    memory_budget_ = reactor_->MemoryBudget();
  context_.client_fd = Fd();
  sockaddr_storage peer{};
  socklen_t peer_len = sizeof(peer);
  if (::getpeername(Fd(), reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0) {
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
  // fd_ closes itself: the guard is the single owner of the client socket.
}

std::chrono::steady_clock::time_point ReactorConnection::LastActive() const {
  return last_active_.load(std::memory_order_relaxed);
}

std::chrono::steady_clock::time_point ReactorConnection::CreatedAt() const {
  return created_at_.load(std::memory_order_relaxed);
}

bool ReactorConnection::OnReadable() {
  last_active_.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);

  std::array<char, kReadChunkBytes> chunk{};
  bool queued = false;
  bool pause_read = false;
  size_t read_bytes = 0;
  while (!read_eof_.load(std::memory_order_acquire)) {
    const ssize_t read = ::recv(Fd(), chunk.data(), chunk.size(), 0);
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
            const auto frame = FindRequestFrame(accumulated_);
            if (!frame.has_value())
              break;
            const size_t end = frame->request_size;
            const size_t delimiter = frame->delimiter_size;
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
            // Under read_mutex_, or DrainRequests could re-enable reads before this disable lands.
            reactor_->SetReadEnabled(Fd(), false);
          }
        }
      }
      if (overflow) {
        spdlog::warn("Reactor request buffer limit reached on fd {}", Fd());
        (void)EnqueueResponse("ERROR Request too large");
        CloseAfterFlush();
        return true;
      }
      if (pause_read)
        break;
      // Level-triggered readiness will report this fd again while the peer
      // still has data. Yield after a bounded turn so one streaming client
      // cannot monopolize the reactor thread.
      if (read_bytes >= kReadEventByteBudget)
        break;
      continue;
    }
    if (read == 0) {
      read_eof_.store(true, std::memory_order_release);
      reactor_->DisarmRead(Fd());
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
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    while (!responses_.empty()) {
      std::string& response = responses_.front();
      const ssize_t sent =
          ::send(Fd(), response.data() + response_offset_, response.size() - response_offset_, kSendFlags);
      if (sent > 0) {
        // A client that keeps reading is not idle, however slowly.
        last_active_.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);
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
    // Under the publication lock, or a response armed after the empty check would be disarmed.
    reactor_->DisarmWrite(Fd());
  }
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
    // Stay registered so the reply is flushed before the close, as on overflow.
    if (!EnqueueResponse("ERROR Server busy"))
      return false;
    CloseAfterFlush();
  }
  return true;
}

void ReactorConnection::DrainRequests() {
  while (true) {
    std::string request;
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
        // Ordered against the high-watermark disable in OnReadable().
        reactor_->SetReadEnabled(Fd(), true);
      }
    }
    std::string response;
    try {
      response = processor_(request, context_);
    } catch (const std::exception& error) {
      spdlog::error("Request processor failed on fd {}: {}", Fd(), error.what());
      response = "ERROR Internal server error";
    } catch (...) {
      spdlog::error("Request processor failed on fd {} with an unknown exception", Fd());
      response = "ERROR Internal server error";
    }
    const bool enqueued = EnqueueResponse(response);
    // Idle time starts when a request completes, not when it was read.
    last_active_.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);
    if (!enqueued) {
      CloseAfterFlush();
      // The close is final: drop the backlog so the connection stops counting as busy.
      std::lock_guard<std::mutex> lock(read_mutex_);
      if (memory_budget_ != nullptr)
        memory_budget_->Release(request_bytes_);
      request_bytes_ = 0;
      requests_.clear();
      drain_scheduled_.store(false, std::memory_order_release);
      return;
    }
  }
  if (read_eof_.load(std::memory_order_acquire) && !HasPendingRequests()) {
    CloseAfterFlush();
    if (!HasPendingOutput())
      reactor_->Unregister(Fd(), this);
  }
}

bool ReactorConnection::EnqueueResponse(const std::string& response) {
  std::string framed = nvecd::server::NormalizeResponse(response);
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (response_bytes_ + framed.size() > kMaxWriteQueueBytes ||
        (memory_budget_ != nullptr && !memory_budget_->TryAcquire(framed.size()))) {
      spdlog::warn("Reactor write buffer limit reached on fd {}", Fd());
      return false;
    }
    response_bytes_ += framed.size();
    responses_.push_back(std::move(framed));
  }
  reactor_->ArmWrite(Fd());
  return true;
}

bool ReactorConnection::HasPendingOutput() const {
  std::lock_guard<std::mutex> lock(write_mutex_);
  return !responses_.empty();
}

bool ReactorConnection::HasRequestsInFlight() const {
  return drain_scheduled_.load(std::memory_order_acquire) || HasPendingRequests();
}

bool ReactorConnection::HasPendingRequests() const {
  std::lock_guard<std::mutex> lock(read_mutex_);
  return !requests_.empty();
}

void ReactorConnection::CloseAfterFlush() {
  close_after_flush_.store(true, std::memory_order_release);
  reactor_->DisarmRead(Fd());
  reactor_->ArmWrite(Fd());
}

}  // namespace nvecd::server

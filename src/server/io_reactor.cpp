/** @file io_reactor.cpp */

#include "server/io_reactor.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>
#include <vector>

#include "server/reactor_connection.h"

namespace nvecd::server {

IoReactor::IoReactor(ReactorConfig config)
    : config_(config), memory_budget_(std::make_shared<ReactorMemoryBudget>(config_.max_total_buffered_bytes)) {}

IoReactor::~IoReactor() {
  Stop();
}

utils::Expected<void, utils::Error> IoReactor::Start() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (running_.load(std::memory_order_acquire))
    return {};

  auto multiplexer = reactor::CreateEventMultiplexer();
  if (!multiplexer) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kNotImplemented, "no supported readiness multiplexer on this platform"));
  }
  if (auto result = multiplexer->Open(); !result)
    return result;

  {
    std::lock_guard<std::mutex> connections_lock(connections_mutex_);
    connections_.clear();
    tokens_.clear();
  }
  multiplexer_ = std::move(multiplexer);
  running_.store(true, std::memory_order_release);
  event_loop_ = std::thread(&IoReactor::EventLoop, this);
  spdlog::info("I/O reactor started ({})", multiplexer_->Name());
  return {};
}

void IoReactor::Stop() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (!running_.exchange(false, std::memory_order_acq_rel))
    return;
  if (multiplexer_) {
    (void)multiplexer_->Wake();
  }
  if (event_loop_.joinable())
    event_loop_.join();

  std::vector<int> fds;
  {
    std::lock_guard<std::mutex> connections_lock(connections_mutex_);
    fds.reserve(connections_.size());
    for (const auto& [fd, entry] : connections_) {
      (void)entry;
      fds.push_back(fd);
    }
  }
  for (int fd : fds)
    Unregister(fd);
  spdlog::info("I/O reactor stopped");
}

utils::Expected<void, utils::Error> IoReactor::Register(std::shared_ptr<ReactorConnection> connection) {
  // Serialize registration with Stop() all the way through poller publication.
  // Without this, a late Register could insert an fd after Stop() snapshotted
  // the map, leaving it alive beyond the server lifecycle.
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (!connection || !running_.load(std::memory_order_acquire)) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCancelled, "reactor is not running"));
  }
  const int fd = connection->Fd();
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kIOError, "failed to set client socket non-blocking"));
  }

  reactor::RegistrationToken token = 0;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    if (connections_.find(fd) != connections_.end()) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kAlreadyExists, "client fd is already registered"));
    }
    token = next_token_++;
    if (token == 0)
      token = next_token_++;
    connections_.emplace(fd, Entry{std::move(connection), token, reactor::event::kReadable});
    tokens_.emplace(token, fd);
  }
  auto result = multiplexer_->Add(fd, reactor::event::kReadable, token);
  if (!result) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_.erase(fd);
    tokens_.erase(token);
    return result;
  }
  return {};
}

void IoReactor::Unregister(int fd, const ReactorConnection* expected_owner) {
  std::shared_ptr<ReactorConnection> connection;
  std::function<void(int)> close_callback;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    const auto found = connections_.find(fd);
    if (found == connections_.end() || (expected_owner != nullptr && found->second.connection.get() != expected_owner))
      return;
    connection = std::move(found->second.connection);
    tokens_.erase(found->second.token);
    connections_.erase(found);
    close_callback = close_callback_;
  }
  if (multiplexer_)
    (void)multiplexer_->Remove(fd);
  if (close_callback)
    close_callback(fd);
}

void IoReactor::ArmWrite(int fd) {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  const auto found = connections_.find(fd);
  if (found == connections_.end() || !running_.load(std::memory_order_acquire))
    return;
  const uint8_t wanted = found->second.interest | reactor::event::kWritable;
  if (wanted == found->second.interest)
    return;
  auto result = multiplexer_->Modify(fd, wanted, found->second.token);
  if (result)
    found->second.interest = wanted;
  else
    spdlog::warn("I/O reactor failed to arm write for fd {}: {}", fd, result.error().message());
}

void IoReactor::DisarmWrite(int fd) {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  const auto found = connections_.find(fd);
  if (found == connections_.end() || !running_.load(std::memory_order_acquire))
    return;
  const uint8_t wanted = found->second.interest & static_cast<uint8_t>(~reactor::event::kWritable);
  if (wanted == found->second.interest)
    return;
  auto result = multiplexer_->Modify(fd, wanted, found->second.token);
  if (result)
    found->second.interest = wanted;
  else
    spdlog::warn("I/O reactor failed to disarm write for fd {}: {}", fd, result.error().message());
}

void IoReactor::DisarmRead(int fd) {
  SetReadEnabled(fd, false);
}

void IoReactor::SetReadEnabled(int fd, bool enabled) {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  const auto found = connections_.find(fd);
  if (found == connections_.end() || !running_.load(std::memory_order_acquire))
    return;
  const uint8_t wanted = enabled ? static_cast<uint8_t>(found->second.interest | reactor::event::kReadable)
                                 : static_cast<uint8_t>(found->second.interest & ~reactor::event::kReadable);
  if (wanted == found->second.interest)
    return;
  auto result = multiplexer_->Modify(fd, wanted, found->second.token);
  if (result)
    found->second.interest = wanted;
  else
    spdlog::warn("I/O reactor failed to update read interest for fd {}: {}", fd, result.error().message());
}

void IoReactor::SetCloseCallback(std::function<void(int)> callback) {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  close_callback_ = std::move(callback);
}

size_t IoReactor::ConnectionCount() const {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  return connections_.size();
}

std::shared_ptr<ReactorConnection> IoReactor::Find(reactor::RegistrationToken token, int fd) const {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  if (fd < 0) {
    const auto by_token = tokens_.find(token);
    if (by_token == tokens_.end())
      return nullptr;
    fd = by_token->second;
  }
  const auto found = connections_.find(fd);
  if (found == connections_.end() || found->second.token != token)
    return nullptr;
  return found->second.connection;
}

void IoReactor::EventLoop() {
  std::vector<reactor::ReadyEvent> ready;
  ready.reserve(64);
  while (running_.load(std::memory_order_acquire)) {
    auto result = multiplexer_->Poll(config_.poll_timeout_ms, ready);
    if (!result) {
      spdlog::warn("I/O reactor poll failed: {}", result.error().message());
      continue;
    }
    for (const auto& event : ready) {
      auto connection = Find(event.token, event.fd);
      if (!connection)
        continue;
      bool keep = true;
      if ((event.events & reactor::event::kReadable) != 0U)
        keep = connection->OnReadable();
      if (keep && (event.events & reactor::event::kWritable) != 0U)
        keep = connection->OnWritable();
      if (keep && (event.events & (reactor::event::kError | reactor::event::kHangup)) != 0U &&
          (event.events & reactor::event::kReadable) == 0U) {
        keep = connection->OnError();
      }
      if (!keep)
        Unregister(connection->Fd(), connection.get());
    }
    if ((config_.idle_timeout_sec > 0 || config_.initial_read_timeout_sec > 0) && config_.reaper_interval_sec > 0 &&
        std::chrono::steady_clock::now() - last_reaper_run_ >= std::chrono::seconds(config_.reaper_interval_sec)) {
      last_reaper_run_ = std::chrono::steady_clock::now();
      ReapIdleConnections();
    }
  }
}

void IoReactor::ReapIdleConnections() {
  if (config_.idle_timeout_sec <= 0 && config_.initial_read_timeout_sec <= 0)
    return;
  const auto now = std::chrono::steady_clock::now();
  const auto cutoff = now - std::chrono::seconds(config_.idle_timeout_sec);
  std::vector<std::pair<int, std::shared_ptr<ReactorConnection>>> stale;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (const auto& [fd, entry] : connections_) {
      if ((config_.initial_read_timeout_sec > 0 && !entry.connection->HasReceivedFrame() &&
           now - entry.connection->CreatedAt() >= std::chrono::seconds(config_.initial_read_timeout_sec)) ||
          (config_.idle_timeout_sec > 0 && entry.connection->LastActive() < cutoff)) {
        stale.emplace_back(fd, entry.connection);
      }
    }
  }
  for (const auto& [fd, connection] : stale)
    Unregister(fd, connection.get());
}

}  // namespace nvecd::server

/**
 * @file event_store.cpp
 * @brief Event store implementation
 */

#include "events/event_store.h"

#include <chrono>

#include "utils/error.h"
#include "utils/structured_log.h"

namespace nvecd::events {

EventStore::EventStore(const config::EventsConfig& config) : config_(config) {}

utils::Expected<void, utils::Error> EventStore::AddEvent(const std::string& ctx, const std::string& item_id,
                                                         int score) {
  // Validate inputs
  if (ctx.empty()) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument, "Context cannot be empty");
    utils::LogEventStoreError("add_event", ctx, error.message());
    return utils::MakeUnexpected(error);
  }

  if (item_id.empty()) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument, "ID cannot be empty");
    utils::LogEventStoreError("add_event", ctx, error.message());
    return utils::MakeUnexpected(error);
  }

  // Get current timestamp
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

  // Create event
  Event event(item_id, score, static_cast<uint64_t>(timestamp));

  // Add to context's ring buffer
  {
    std::unique_lock lock(mutex_);

    // Create ring buffer for context if it doesn't exist
    auto iter = ctx_events_.find(ctx);
    if (iter == ctx_events_.end()) {
      auto [new_iter, inserted] = ctx_events_.emplace(ctx, RingBuffer<Event>(config_.ctx_buffer_size));
      iter = new_iter;
    }

    // Add event to ring buffer
    iter->second.Push(event);
  }

  // Increment total event count
  total_events_.fetch_add(1, std::memory_order_relaxed);

  return {};
}

std::vector<Event> EventStore::GetEvents(const std::string& ctx) const {
  std::shared_lock lock(mutex_);

  auto iter = ctx_events_.find(ctx);
  if (iter == ctx_events_.end()) {
    return {};
  }

  return iter->second.GetAll();
}

size_t EventStore::GetContextCount() const {
  std::shared_lock lock(mutex_);
  return ctx_events_.size();
}

std::vector<std::string> EventStore::GetAllContexts() const {
  std::shared_lock lock(mutex_);

  std::vector<std::string> contexts;
  contexts.reserve(ctx_events_.size());

  for (const auto& [ctx, _] : ctx_events_) {
    contexts.push_back(ctx);
  }

  return contexts;
}

void EventStore::Clear() {
  std::unique_lock lock(mutex_);
  ctx_events_.clear();
  total_events_.store(0, std::memory_order_relaxed);
}

EventStoreStatistics EventStore::GetStatistics() const {
  std::shared_lock lock(mutex_);

  EventStoreStatistics stats;
  stats.active_contexts = ctx_events_.size();
  stats.total_events = total_events_.load(std::memory_order_relaxed);

  // Count currently stored events
  size_t stored = 0;
  for (const auto& [ctx, ring_buffer] : ctx_events_) {
    stored += ring_buffer.Size();
  }
  stats.stored_events = stored;

  // Estimate memory usage
  stats.memory_bytes = MemoryUsage();

  return stats;
}

size_t EventStore::MemoryUsage() const {
  std::shared_lock lock(mutex_);

  size_t total = 0;

  // Base container overhead
  total += sizeof(*this);

  // Ring buffers and context strings
  for (const auto& [ctx, ring_buffer] : ctx_events_) {
    // Context string
    total += sizeof(std::string) + ctx.capacity();

    // Ring buffer events
    auto events = ring_buffer.GetAll();
    for (const auto& event : events) {
      total += sizeof(Event);
      total += event.item_id.capacity();  // Event ID string
    }

    // Ring buffer overhead
    total += sizeof(RingBuffer<Event>);
    total += config_.ctx_buffer_size * sizeof(Event);  // Allocated capacity
  }

  return total;
}

}  // namespace nvecd::events

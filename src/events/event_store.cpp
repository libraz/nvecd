/**
 * @file event_store.cpp
 * @brief Event store implementation
 */

#include "events/event_store.h"

#include <chrono>

#include "utils/error.h"
#include "utils/structured_log.h"

namespace nvecd::events {

EventStore::EventStore(const config::EventsConfig& config) : config_(config) {
  // Initialize deduplication cache for ADD type (time-window based)
  if (config_.dedup_window_sec > 0 && config_.dedup_cache_size > 0) {
    dedup_cache_ = std::make_unique<DedupCache>(config_.dedup_cache_size,
                                                 config_.dedup_window_sec);
  }

  // Initialize state cache for SET/DEL type (last-value based)
  if (config_.dedup_cache_size > 0) {
    state_cache_ = std::make_unique<StateCache>(config_.dedup_cache_size);
  }
}

utils::Expected<void, utils::Error> EventStore::AddEvent(
    const std::string& ctx, const std::string& id, int score, EventType type) {
  // Validate inputs
  if (ctx.empty()) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument,
                                  "Context cannot be empty");
    utils::LogEventStoreError("add_event", ctx, error.message());
    return utils::MakeUnexpected(error);
  }

  if (id.empty()) {
    auto error =
        utils::MakeError(utils::ErrorCode::kInvalidArgument, "ID cannot be empty");
    utils::LogEventStoreError("add_event", ctx, error.message());
    return utils::MakeUnexpected(error);
  }

  // Get current timestamp
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                       now.time_since_epoch())
                       .count();
  uint64_t ts = static_cast<uint64_t>(timestamp);

  // Increment total event count (includes duplicates)
  total_events_.fetch_add(1, std::memory_order_relaxed);

  // Deduplication based on event type
  switch (type) {
    case EventType::ADD:
      // Time-window based deduplication
      if (dedup_cache_) {
        EventKey key(ctx, id, score);
        if (dedup_cache_->IsDuplicate(key, ts)) {
          deduped_events_.fetch_add(1, std::memory_order_relaxed);
          return {};  // Duplicate within time window
        }
        dedup_cache_->Insert(key, ts);
      }
      break;

    case EventType::SET:
      // Last-value based deduplication
      if (state_cache_) {
        StateKey key(ctx, id);
        if (state_cache_->IsDuplicateSet(key, score)) {
          deduped_events_.fetch_add(1, std::memory_order_relaxed);
          return {};  // Same value, idempotent skip
        }
        state_cache_->UpdateScore(key, score);
      }
      break;

    case EventType::DEL:
      // Deletion flag based deduplication
      if (state_cache_) {
        StateKey key(ctx, id);
        if (state_cache_->IsDuplicateDel(key)) {
          deduped_events_.fetch_add(1, std::memory_order_relaxed);
          return {};  // Already deleted
        }
        state_cache_->MarkDeleted(key);
      }
      // For DEL, store with score=0
      score = 0;
      break;
  }

  // Create event
  Event event(id, score, ts, type);

  // Add to context's ring buffer
  {
    std::unique_lock lock(mutex_);

    // Create ring buffer for context if it doesn't exist
    auto it = ctx_events_.find(ctx);
    if (it == ctx_events_.end()) {
      auto [new_it, inserted] = ctx_events_.emplace(
          ctx, RingBuffer<Event>(config_.ctx_buffer_size));
      it = new_it;
    }

    // Add event to ring buffer
    it->second.Push(event);
  }

  return {};
}

std::vector<Event> EventStore::GetEvents(const std::string& ctx) const {
  std::shared_lock lock(mutex_);

  auto it = ctx_events_.find(ctx);
  if (it == ctx_events_.end()) {
    return {};
  }

  return it->second.GetAll();
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
  deduped_events_.store(0, std::memory_order_relaxed);
  if (dedup_cache_) {
    dedup_cache_->Clear();
  }
  if (state_cache_) {
    state_cache_->Clear();
  }
}

EventStoreStatistics EventStore::GetStatistics() const {
  std::shared_lock lock(mutex_);

  EventStoreStatistics stats;
  stats.active_contexts = ctx_events_.size();
  stats.total_events = total_events_.load(std::memory_order_relaxed);
  stats.deduped_events = deduped_events_.load(std::memory_order_relaxed);

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

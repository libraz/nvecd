/**
 * @file event_store.cpp
 * @brief Event store implementation
 */

#include "events/event_store.h"

#include <algorithm>
#include <chrono>

#include "utils/error.h"
#include "utils/structured_log.h"

namespace nvecd::events {

EventStore::EventStore(const config::EventsConfig& config) : config_(config) {
  // Initialize deduplication cache for ADD type (time-window based)
  if (config_.dedup_window_sec > 0 && config_.dedup_cache_size > 0) {
    dedup_cache_ = std::make_unique<DedupCache>(config_.dedup_cache_size, config_.dedup_window_sec);
  }

  // Initialize state cache for SET/DEL type (last-value based)
  if (config_.dedup_cache_size > 0) {
    state_cache_ = std::make_unique<StateCache>(config_.dedup_cache_size);
  }
}

namespace {

/// @brief Minimum valid event score (inclusive).
constexpr int kMinEventScore = 0;
/// @brief Maximum valid event score (inclusive).
constexpr int kMaxEventScore = 100;

/// @brief Resolve a request timestamp, substituting current time for 0.
uint64_t ResolveTimestamp(uint64_t timestamp) {
  if (timestamp != 0) {
    return timestamp;
  }
  auto now = std::chrono::system_clock::now();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
}

}  // namespace

utils::Expected<void, utils::Error> EventStore::AddEvent(const std::string& ctx, const std::string& id, int score,
                                                         EventType type, uint64_t timestamp) {
  auto result = AddEventAndGetPrior(ctx, id, score, type, timestamp);
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }
  return {};
}

utils::Expected<EventStore::IngestResult, utils::Error> EventStore::AddEventAndGetPrior(const std::string& ctx,
                                                                                        const std::string& id,
                                                                                        int score, EventType type,
                                                                                        uint64_t timestamp) {
  // Validate inputs
  if (ctx.empty()) {
    auto error = utils::MakeError(utils::ErrorCode::kEventStoreError, "Context cannot be empty");
    utils::LogEventStoreError("add_event", ctx, error.message());
    return utils::MakeUnexpected(error);
  }

  if (id.empty()) {
    auto error = utils::MakeError(utils::ErrorCode::kEventStoreError, "ID cannot be empty");
    utils::LogEventStoreError("add_event", ctx, error.message());
    return utils::MakeUnexpected(error);
  }

  // Validate score range defensively. DEL events ignore the score (it is forced
  // to 0 below), so only ADD/SET scores are range-checked. Out-of-range scores
  // are rejected here so the co-occurrence product (score1 * score2) cannot be
  // driven out of its expected [0, 10000] range.
  if (type != EventType::DEL && (score < kMinEventScore || score > kMaxEventScore)) {
    auto error = utils::MakeError(utils::ErrorCode::kEventInvalidScore,
                                  "Score must be in range [0, 100], got " + std::to_string(score));
    utils::LogEventStoreError("add_event", ctx, error.message());
    return utils::MakeUnexpected(error);
  }

  // Use provided timestamp or current time
  uint64_t ts = ResolveTimestamp(timestamp);

  // Increment total event count (includes duplicates)
  total_events_.fetch_add(1, std::memory_order_relaxed);

  IngestResult result;

  // Deduplication based on event type
  switch (type) {
    case EventType::ADD:
      // Time-window based deduplication
      if (dedup_cache_) {
        EventKey key(ctx, id, score);
        if (dedup_cache_->CheckAndInsert(key, ts)) {
          deduped_events_.fetch_add(1, std::memory_order_relaxed);
          result.deduped = true;
          return result;  // Duplicate within time window
        }
      }
      break;

    case EventType::SET:
      // Last-value based deduplication
      if (state_cache_) {
        StateKey key(ctx, id);
        if (state_cache_->CheckAndUpdateSet(key, score)) {
          deduped_events_.fetch_add(1, std::memory_order_relaxed);
          result.deduped = true;
          return result;  // Same value, idempotent skip
        }
      }
      break;

    case EventType::DEL:
      // Deletion flag based deduplication
      if (state_cache_) {
        StateKey key(ctx, id);
        if (state_cache_->CheckAndMarkDeleted(key)) {
          deduped_events_.fetch_add(1, std::memory_order_relaxed);
          result.deduped = true;
          return result;  // Already deleted
        }
      }
      // For DEL, store with score=0
      score = 0;
      break;
  }

  // Create event
  Event event(id, score, ts, type);
  result.stored_event = event;

  // Atomically snapshot the prior buffer state and append the new event.
  // Capturing prior_events under the same lock that performs the push
  // guarantees that concurrent same-context ingests each observe a distinct,
  // consistent prior view (no lost or duplicated co-occurrence pairs).
  {
    std::unique_lock lock(mutex_);

    // Create ring buffer for context if it doesn't exist
    auto it = ctx_events_.find(ctx);
    if (it == ctx_events_.end()) {
      EvictLeastRecentlyUsedContextLocked();
      auto [new_it, inserted] = ctx_events_.emplace(ctx, RingBuffer<Event>(config_.ctx_buffer_size));
      it = new_it;
    }
    TouchContextLocked(ctx);

    // Snapshot the buffer contents that existed before this event.
    result.prior_events = it->second.GetAll();

    // Add event to ring buffer
    it->second.Push(event);
  }

  return result;
}

utils::Expected<void, utils::Error> EventStore::RestoreEvent(const std::string& ctx, const Event& event) {
  if (ctx.empty()) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument, "Context cannot be empty");
    utils::LogEventStoreError("restore_event", ctx, error.message());
    return utils::MakeUnexpected(error);
  }
  if (event.item_id.empty()) {
    auto error = utils::MakeError(utils::ErrorCode::kInvalidArgument, "ID cannot be empty");
    utils::LogEventStoreError("restore_event", ctx, error.message());
    return utils::MakeUnexpected(error);
  }

  total_events_.fetch_add(1, std::memory_order_relaxed);

  std::unique_lock lock(mutex_);
  auto it = ctx_events_.find(ctx);
  if (it == ctx_events_.end()) {
    EvictLeastRecentlyUsedContextLocked();
    auto [new_it, inserted] = ctx_events_.emplace(ctx, RingBuffer<Event>(config_.ctx_buffer_size));
    it = new_it;
  }
  TouchContextLocked(ctx);
  // Push verbatim: preserve the original score, type, and timestamp so
  // temporal-decay weights and DEL/SET semantics survive the reload exactly.
  it->second.Push(event);

  // Restore the last-value state cache in the same insertion order as the
  // ring buffer. Snapshot/WAL recovery bypasses AddEventAndGetPrior(), so
  // without this reseed a replayed SET/DEL would look new and apply its
  // co-occurrence delta a second time after restart.
  if (state_cache_) {
    StateKey key(ctx, event.item_id);
    if (event.type == EventType::SET) {
      state_cache_->UpdateScore(key, event.score);
    } else if (event.type == EventType::DEL) {
      state_cache_->MarkDeleted(key);
    }
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
  ctx_last_access_.clear();
  context_access_sequence_ = 0;
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
  stats.memory_bytes = MemoryUsageLocked();

  return stats;
}

size_t EventStore::MemoryUsage() const {
  std::shared_lock lock(mutex_);
  return MemoryUsageLocked();
}

size_t EventStore::MemoryUsageLocked() const {
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

void EventStore::EvictLeastRecentlyUsedContextLocked() {
  if (config_.max_contexts == 0 || ctx_events_.size() < config_.max_contexts) {
    return;
  }

  const auto victim = std::min_element(ctx_last_access_.begin(), ctx_last_access_.end(),
                                       [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
  if (victim == ctx_last_access_.end()) {
    return;
  }
  ctx_events_.erase(victim->first);
  ctx_last_access_.erase(victim);
}

void EventStore::TouchContextLocked(const std::string& ctx) {
  ctx_last_access_[ctx] = ++context_access_sequence_;
}

std::shared_lock<std::shared_mutex> EventStore::AcquireReadLock() const {
  return std::shared_lock<std::shared_mutex>(mutex_);
}

std::unique_lock<std::shared_mutex> EventStore::AcquireWriteLock() {
  return std::unique_lock<std::shared_mutex>(mutex_);
}

}  // namespace nvecd::events

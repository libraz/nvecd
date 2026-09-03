/**
 * @file event_store.cpp
 * @brief Event store implementation
 */

#include "events/event_store.h"

#include <algorithm>
#include <chrono>

#include "events/id_validation.h"
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

utils::Expected<ValidatedEvent, utils::Error> EventValidator::Validate(const std::string& ctx, const Event& event) {
  auto ctx_valid = ValidateIdentifier("Context", ctx);
  if (!ctx_valid) {
    return utils::MakeUnexpected(ctx_valid.error());
  }
  auto id_valid = ValidateIdentifier("ID", event.item_id);
  if (!id_valid) {
    return utils::MakeUnexpected(id_valid.error());
  }

  // Score range. DEL carries no weight, so it is stored as exactly 0; ADD and
  // SET are bounded so the co-occurrence product (score1 * score2) cannot be
  // driven out of its expected [0, 10000] range.
  if (event.type == EventType::DEL) {
    if (event.score != 0) {
      return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kEventInvalidScore,
                                                    "DEL score must be 0, got " + std::to_string(event.score)));
    }
  } else if (event.score < kMinEventScore || event.score > kMaxEventScore) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kEventInvalidScore, "Score must be in range [0, 100], got " + std::to_string(event.score)));
  }

  return ValidatedEvent(ContextEvent{ctx, event});
}

utils::Expected<void, utils::Error> EventStore::AddEvent(const std::string& ctx, const std::string& id, int score,
                                                         EventType type, uint64_t timestamp) {
  auto result = AddEventAndGetPrior(ctx, id, score, type, timestamp);
  if (!result) {
    return utils::MakeUnexpected(result.error());
  }
  return {};
}

utils::Expected<EventStore::PreparedEvent, utils::Error> EventStore::PrepareEvent(const std::string& ctx,
                                                                                  const std::string& id, int score,
                                                                                  EventType type,
                                                                                  uint64_t timestamp) const {
  const uint64_t resolved_timestamp = ResolveTimestamp(timestamp);
  // DEL carries no weight: it is stored with score 0 whatever the request asked
  // for, so the score is normalized before validation rather than after it.
  const int stored_score = (type == EventType::DEL) ? 0 : score;
  auto validated = EventValidator::Validate(ctx, Event(id, stored_score, resolved_timestamp, type));
  if (!validated) {
    return utils::MakeUnexpected(validated.error());
  }

  PreparedEvent prepared;
  switch (type) {
    case EventType::ADD:
      prepared.deduped =
          dedup_cache_ != nullptr && dedup_cache_->WouldDeduplicate(EventKey(ctx, id, score), resolved_timestamp);
      break;
    case EventType::SET:
      prepared.deduped = state_cache_ != nullptr && state_cache_->WouldDeduplicateSet(StateKey(ctx, id), score);
      break;
    case EventType::DEL:
      prepared.deduped = state_cache_ != nullptr && state_cache_->WouldDeduplicateDel(StateKey(ctx, id));
      break;
  }
  prepared.event = validated->Get().event;
  return prepared;
}

utils::Expected<EventStore::IngestResult, utils::Error> EventStore::AddEventAndGetPrior(const std::string& ctx,
                                                                                        const std::string& id,
                                                                                        int score, EventType type,
                                                                                        uint64_t timestamp) {
  // Use provided timestamp or current time
  const uint64_t ts = ResolveTimestamp(timestamp);
  // DEL events carry no weight and are stored with score 0.
  const int stored_score = (type == EventType::DEL) ? 0 : score;

  auto validated = EventValidator::Validate(ctx, Event(id, stored_score, ts, type));
  if (!validated) {
    utils::LogEventStoreError("add_event", ctx, validated.error().message());
    return utils::MakeUnexpected(validated.error());
  }

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
      break;
  }

  result.stored_event = validated->Get().event;
  AppendValidated(*validated, &result.prior_events);

  return result;
}

utils::Expected<void, utils::Error> EventStore::RestoreEvent(const std::string& ctx, const Event& event) {
  // Same validator as the live path: the score value, type and timestamp are
  // preserved verbatim, but an untrusted snapshot or WAL record cannot use that
  // to plant something the live path would have refused.
  auto validated = EventValidator::Validate(ctx, event);
  if (!validated) {
    utils::LogEventStoreError("restore_event", ctx, validated.error().message());
    return utils::MakeUnexpected(validated.error());
  }

  total_events_.fetch_add(1, std::memory_order_relaxed);
  AppendValidated(*validated, nullptr);

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
  ctx_lru_.clear();
  ctx_lru_pos_.clear();
  total_events_.store(0, std::memory_order_relaxed);
  deduped_events_.store(0, std::memory_order_relaxed);
  if (dedup_cache_) {
    dedup_cache_->Clear();
  }
  if (state_cache_) {
    state_cache_->Clear();
  }
}

void EventStore::SwapState(EventStore& other) {
  if (this == &other) {
    return;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  ctx_events_.swap(other.ctx_events_);
  // std::list::swap keeps iterators valid, so the position map stays paired
  // with its list as long as both are swapped together.
  ctx_lru_.swap(other.ctx_lru_);
  ctx_lru_pos_.swap(other.ctx_lru_pos_);

  const uint64_t this_total = total_events_.load(std::memory_order_relaxed);
  total_events_.store(other.total_events_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  other.total_events_.store(this_total, std::memory_order_relaxed);
  const uint64_t this_deduped = deduped_events_.load(std::memory_order_relaxed);
  deduped_events_.store(other.deduped_events_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  other.deduped_events_.store(this_deduped, std::memory_order_relaxed);
  dedup_cache_.swap(other.dedup_cache_);
  state_cache_.swap(other.state_cache_);
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

    // Ring buffer events. Traversed in place: materializing them with GetAll()
    // would deep-copy every stored event (and its heap-allocated item_id) on
    // every statistics or metrics read, while the lock is held.
    ring_buffer.ForEach([&total](const Event& event) {
      total += sizeof(Event);
      total += event.item_id.capacity();  // Event ID string
    });

    // Ring buffer overhead
    total += sizeof(RingBuffer<Event>);
    total += config_.ctx_buffer_size * sizeof(Event);  // Allocated capacity
  }

  return total;
}

void EventStore::AppendValidated(const ValidatedEvent& validated, std::vector<Event>* prior_events) {
  const ContextEvent& pair = *validated;

  // Capturing prior_events under the same lock that performs the push
  // guarantees that concurrent same-context ingests each observe a distinct,
  // consistent prior view (no lost or duplicated co-occurrence pairs).
  std::unique_lock lock(mutex_);

  // Create ring buffer for context if it doesn't exist
  auto it = ctx_events_.find(pair.ctx);
  if (it == ctx_events_.end()) {
    EvictLeastRecentlyUsedContextLocked();
    auto [new_it, inserted] = ctx_events_.emplace(pair.ctx, RingBuffer<Event>(config_.ctx_buffer_size));
    it = new_it;
  }
  TouchContextLocked(pair.ctx);

  if (prior_events != nullptr) {
    // Snapshot the buffer contents that existed before this event.
    *prior_events = it->second.GetAll();
  }

  it->second.Push(pair.event);
}

void EventStore::EvictLeastRecentlyUsedContextLocked() {
  if (config_.max_contexts == 0 || ctx_events_.size() < config_.max_contexts) {
    return;
  }
  if (ctx_lru_.empty()) {
    return;
  }

  const std::string victim = ctx_lru_.front();
  ctx_events_.erase(victim);
  ctx_lru_pos_.erase(victim);
  ctx_lru_.pop_front();
}

void EventStore::TouchContextLocked(const std::string& ctx) {
  auto pos = ctx_lru_pos_.find(ctx);
  if (pos == ctx_lru_pos_.end()) {
    ctx_lru_pos_.emplace(ctx, ctx_lru_.insert(ctx_lru_.end(), ctx));
    return;
  }
  // Splice keeps the iterator (and therefore ctx_lru_pos_) valid.
  ctx_lru_.splice(ctx_lru_.end(), ctx_lru_, pos->second);
}

std::shared_lock<std::shared_mutex> EventStore::AcquireReadLock() const {
  return std::shared_lock<std::shared_mutex>(mutex_);
}

std::unique_lock<std::shared_mutex> EventStore::AcquireWriteLock() {
  return std::unique_lock<std::shared_mutex>(mutex_);
}

}  // namespace nvecd::events

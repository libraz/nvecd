/**
 * @file event_store.h
 * @brief Event store with per-context ring buffers
 *
 * Stores recent events for each context in fixed-size ring buffers.
 * Thread-safe for concurrent reads and writes.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "config/config.h"
#include "events/dedup_cache.h"
#include "events/ring_buffer.h"
#include "events/state_cache.h"
#include "events/validated.h"
#include "utils/error.h"
#include "utils/expected.h"

// nameser.h (included via resolv.h on Linux) defines ADD/DELETE macros
// that conflict with our enum values. Undefine them here.
#ifdef ADD
#undef ADD
#endif
#ifdef DELETE
#undef DELETE
#endif

namespace nvecd::events {

/**
 * @brief Event type enumeration
 *
 * - ADD: Stream events (clicks, views) - time-window deduplication
 * - SET: State events (likes, bookmarks) - last-value deduplication
 * - DEL: Deletion events (unlike, unbookmark) - deletion-flag deduplication
 */
enum class EventType : std::uint8_t {
  ADD,  ///< Stream event (default)
  SET,  ///< State event
  DEL   ///< Deletion event
};

/**
 * @brief Event data structure
 */
struct Event {
  std::string item_id;             ///< Event ID (e.g., item ID)
  int score{0};                    ///< Event score/weight
  uint64_t timestamp{0};           ///< Unix timestamp (seconds)
  EventType type{EventType::ADD};  ///< Event type

  Event() = default;
  Event(std::string item_id_, int score_, uint64_t timestamp_, EventType type_ = EventType::ADD)
      : item_id(std::move(item_id_)), score(score_), timestamp(timestamp_), type(type_) {}
};

class EventValidator;

/**
 * @brief An event together with the context it belongs to
 */
struct ContextEvent {
  std::string ctx;  ///< Context identifier
  Event event;      ///< Event as it will be stored
};

/// @brief A (context, event) pair that satisfies every event store invariant.
using ValidatedEvent = Validated<ContextEvent, EventValidator>;

/**
 * @brief Sole producer of ValidatedEvent
 *
 * Wire ingestion (EventStore::PrepareEvent, EventStore::AddEventAndGetPrior)
 * and snapshot/WAL restore (EventStore::RestoreEvent) both mint here, so an
 * input the live path rejects is rejected by the restore path as well.
 * Restore keeps the original score value, type and timestamp; what it does not
 * get is an exemption from the invariants those values must satisfy.
 */
class EventValidator {
 public:
  /**
   * @brief Check the store invariants and mint a ValidatedEvent
   *
   * The event must already be in its final stored form: the timestamp resolved
   * and, for DEL, the score forced to 0. Nothing here rewrites the event.
   *
   * @param ctx Context identifier
   * @param event Event in its final stored form
   * @return Expected<ValidatedEvent, Error> The validated pair or the reason it
   *         was rejected
   */
  static utils::Expected<ValidatedEvent, utils::Error> Validate(const std::string& ctx, const Event& event);
};

/**
 * @brief Event store statistics
 */
struct EventStoreStatistics {
  size_t active_contexts = 0;   ///< Number of contexts with events
  uint64_t total_events = 0;    ///< Total events processed (cumulative)
  uint64_t deduped_events = 0;  ///< Total deduplicated events (ignored)
  size_t stored_events = 0;     ///< Current number of stored events
  size_t memory_bytes = 0;      ///< Estimated memory usage in bytes
};

/**
 * @brief Event store with per-context ring buffers
 *
 * Stores recent events for each context in fixed-size ring buffers.
 * Supports concurrent reads and writes using reader-writer locks.
 *
 * Thread-safety:
 * - Multiple concurrent readers (GetEvents, GetContextCount, etc.)
 * - Exclusive writer (AddEvent)
 *
 * Example:
 * @code
 * EventStore store(config);
 * auto result = store.AddEvent("user123", "item456", 95);
 * if (result) {
 *   auto events = store.GetEvents("user123");
 *   // Process events...
 * }
 * @endcode
 */
class EventStore {
 public:
  /**
   * @brief Construct event store with configuration
   * @param config Event store configuration
   */
  explicit EventStore(const config::EventsConfig& config);

  /**
   * @brief Add an event to a context
   *
   * If the context's ring buffer is full, overwrites the oldest event.
   *
   * @param ctx Context identifier (e.g., user ID, session ID)
   * @param item_id Event ID (e.g., item ID)
   * @param score Event score/weight
   * @param type Event type (ADD/SET/DEL)
   * @param timestamp Unix timestamp in seconds (0 = use current time)
   * @return Expected<void, Error> Success or error
   *
   * @throws None
   */
  utils::Expected<void, utils::Error> AddEvent(const std::string& ctx, const std::string& item_id, int score,
                                               EventType type = EventType::ADD, uint64_t timestamp = 0);

  /**
   * @brief Result of an atomic event ingestion
   *
   * Describes what happened when an event was offered to the store so the
   * caller can apply the matching incremental co-occurrence delta.
   */
  struct IngestResult {
    bool deduped = false;             ///< True if the event was a duplicate and not stored
    Event stored_event;               ///< The event as stored (populated when !deduped)
    std::vector<Event> prior_events;  ///< Context buffer contents immediately before the append
  };

  struct PreparedEvent {
    bool deduped = false;
    Event event;
  };

  /** Validate and preview an event without mutating store or dedup state. */
  utils::Expected<PreparedEvent, utils::Error> PrepareEvent(const std::string& ctx, const std::string& item_id,
                                                            int score, EventType type = EventType::ADD,
                                                            uint64_t timestamp = 0) const;

  /**
   * @brief Atomically append an event and capture the prior buffer state
   *
   * Performs the same validation and deduplication as AddEvent(), then, under
   * a single critical section, snapshots the context's existing events and
   * pushes the new one. The returned prior_events reflect the buffer state
   * exactly as it was immediately before this event's append, so the caller
   * can compute the incremental co-occurrence delta from a consistent view
   * even under concurrent ingestion of the same context.
   *
   * @param ctx Context identifier (e.g., user ID, session ID)
   * @param item_id Event ID (e.g., item ID)
   * @param score Event score/weight
   * @param type Event type (ADD/SET/DEL)
   * @param timestamp Unix timestamp in seconds (0 = use current time)
   * @return Expected<IngestResult, Error> Ingestion outcome or error
   */
  utils::Expected<IngestResult, utils::Error> AddEventAndGetPrior(const std::string& ctx, const std::string& item_id,
                                                                  int score, EventType type = EventType::ADD,
                                                                  uint64_t timestamp = 0);

  /**
   * @brief Restore an event verbatim into a context buffer (snapshot load only)
   *
   * Appends @p event to the context's ring buffer exactly as given, preserving
   * its item_id, score, type, and timestamp. Unlike AddEvent(), this bypasses
   * deduplication and the dedup/state caches so a serialized buffer reloads
   * byte-for-byte: temporal-decay weights depend on the original timestamps and
   * DEL/SET semantics depend on the original types, neither of which must be
   * altered by replaying through the dedup path. The total event counter is
   * incremented to mirror the original ingestion count.
   *
   * What is restored verbatim is the value, not an exemption from the store's
   * invariants: the event goes through EventValidator just like a wire event,
   * so an untrusted snapshot or a tampered WAL record cannot plant a score, a
   * type or an identifier the live path would have refused.
   *
   * Events MUST be restored in their original insertion order (oldest first)
   * so the ring buffer's eviction order matches the snapshot.
   *
   * @param ctx Context identifier
   * @param event Event to append verbatim
   * @return Expected<void, Error> Success or error (empty ctx / empty item_id)
   */
  utils::Expected<void, utils::Error> RestoreEvent(const std::string& ctx, const Event& event);

  /**
   * @brief Get all events for a context
   *
   * Returns events in insertion order (oldest to newest).
   *
   * @param ctx Context identifier
   * @return Vector of events (empty if context not found)
   */
  std::vector<Event> GetEvents(const std::string& ctx) const;

  /**
   * @brief Get number of tracked contexts
   * @return Number of contexts with at least one event
   */
  size_t GetContextCount() const;

  /**
   * @brief Get total number of events processed
   *
   * This is the cumulative count of all AddEvent calls, not the current
   * number of stored events (which may be less due to ring buffer overwrite).
   *
   * @return Total number of events processed
   */
  uint64_t GetTotalEventCount() const { return total_events_.load(); }

  /**
   * @brief Get all context identifiers
   * @return Vector of all context IDs
   */
  std::vector<std::string> GetAllContexts() const;

  /**
   * @brief Clear all events from all contexts
   */
  void Clear();

  /**
   * @brief Atomically exchange snapshot-managed state with another store
   * @param other Staged store populated by snapshot deserialization
   * @note Configuration is not exchanged; both stores must use compatible configuration.
   */
  void SwapState(EventStore& other);

  /**
   * @brief Get event store statistics
   * @return Statistics snapshot
   */
  EventStoreStatistics GetStatistics() const;

  /**
   * @brief Get estimated memory usage in bytes
   * @return Estimated memory usage
   */
  size_t MemoryUsage() const;

  /**
   * @brief Acquire read lock for snapshot consistency
   * @return Shared lock guard
   */
  std::shared_lock<std::shared_mutex> AcquireReadLock() const;

  /**
   * @brief Acquire write lock for snapshot consistency
   * @return Unique lock guard
   */
  std::unique_lock<std::shared_mutex> AcquireWriteLock();

 private:
  /**
   * @brief Estimate memory usage assuming the caller already holds mutex_
   *
   * Split out from MemoryUsage() so callers that already hold the lock (e.g.
   * GetStatistics()) do not re-acquire the non-recursive shared_mutex, which
   * would be undefined behavior and can deadlock a writer-preferring lock.
   *
   * @return Estimated memory usage in bytes
   */
  size_t MemoryUsageLocked() const;

  /**
   * @brief Append a validated event, optionally capturing the prior buffer
   *
   * The only path that writes into a context ring buffer. Taking a
   * ValidatedEvent means no caller can reach the buffers with a raw value that
   * has not been through EventValidator.
   *
   * @param validated Event to append
   * @param prior_events When non-null, receives the buffer contents captured
   *        under the same lock immediately before the append
   */
  void AppendValidated(const ValidatedEvent& validated, std::vector<Event>* prior_events);

  /**
   * @brief Make room for a new context according to the configured LRU cap.
   * @pre mutex_ is held exclusively and @p ctx is not already present.
   */
  void EvictLeastRecentlyUsedContextLocked();

  /// Record context activity while holding mutex_ exclusively.
  void TouchContextLocked(const std::string& ctx);

  config::EventsConfig config_;  ///< Configuration

  // Context -> RingBuffer<Event>
  // Uses shared_mutex for reader-writer lock pattern
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, RingBuffer<Event>> ctx_events_;

  // Write recency as an intrusive LRU list: least recently written context at
  // the front, one iterator per context. Eviction pops the front instead of
  // scanning, so the exclusive lock is held for a time independent of
  // max_contexts (which the schema allows to reach 1,000,000).
  std::list<std::string> ctx_lru_;
  std::unordered_map<std::string, std::list<std::string>::iterator> ctx_lru_pos_;

  std::atomic<uint64_t> total_events_{0};    ///< Total events processed
  std::atomic<uint64_t> deduped_events_{0};  ///< Total deduplicated events

  // Deduplication caches
  std::unique_ptr<DedupCache> dedup_cache_;  ///< For ADD type (time-window)
  std::unique_ptr<StateCache> state_cache_;  ///< For SET/DEL type (last-value)
};

}  // namespace nvecd::events

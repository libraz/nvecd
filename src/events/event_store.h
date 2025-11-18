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
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "config/config.h"
#include "events/dedup_cache.h"
#include "events/ring_buffer.h"
#include "events/state_cache.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::events {

/**
 * @brief Event type enumeration
 *
 * - ADD: Stream events (clicks, views) - time-window deduplication
 * - SET: State events (likes, bookmarks) - last-value deduplication
 * - DEL: Deletion events (unlike, unbookmark) - deletion-flag deduplication
 */
enum class EventType {
  ADD,  ///< Stream event (default)
  SET,  ///< State event
  DEL   ///< Deletion event
};

/**
 * @brief Event data structure
 */
struct Event {
  std::string item_id;    ///< Event ID (e.g., item ID)
  int score{0};           ///< Event score/weight
  uint64_t timestamp{0};  ///< Unix timestamp (seconds)
  EventType type;         ///< Event type

  Event() = default;
  Event(std::string item_id_, int score_, uint64_t timestamp_, EventType type_ = EventType::ADD)
      : item_id(std::move(item_id_)), score(score_), timestamp(timestamp_), type(type_) {}
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
   * @return Expected<void, Error> Success or error
   *
   * @throws None
   */
  utils::Expected<void, utils::Error> AddEvent(const std::string& ctx, const std::string& item_id, int score,
                                                EventType type = EventType::ADD);

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
   * @brief Get event store statistics
   * @return Statistics snapshot
   */
  EventStoreStatistics GetStatistics() const;

  /**
   * @brief Get estimated memory usage in bytes
   * @return Estimated memory usage
   */
  size_t MemoryUsage() const;

 private:
  config::EventsConfig config_;  ///< Configuration

  // Context -> RingBuffer<Event>
  // Uses shared_mutex for reader-writer lock pattern
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, RingBuffer<Event>> ctx_events_;

  std::atomic<uint64_t> total_events_{0};    ///< Total events processed
  std::atomic<uint64_t> deduped_events_{0};  ///< Total deduplicated events

  // Deduplication caches
  std::unique_ptr<DedupCache> dedup_cache_;  ///< For ADD type (time-window)
  std::unique_ptr<StateCache> state_cache_;  ///< For SET/DEL type (last-value)
};

}  // namespace nvecd::events

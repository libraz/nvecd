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
#include "events/ring_buffer.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::events {

/**
 * @brief Event data structure
 */
struct Event {
  std::string id;       ///< Event ID (e.g., item ID)
  int score;            ///< Event score/weight
  uint64_t timestamp;   ///< Unix timestamp (seconds)

  Event() = default;
  Event(std::string id_, int score_, uint64_t timestamp_)
      : id(std::move(id_)), score(score_), timestamp(timestamp_) {}
};

/**
 * @brief Event store statistics
 */
struct EventStoreStatistics {
  size_t active_contexts = 0;     ///< Number of contexts with events
  uint64_t total_events = 0;      ///< Total events processed (cumulative)
  size_t stored_events = 0;       ///< Current number of stored events
  size_t memory_bytes = 0;        ///< Estimated memory usage in bytes
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
   * @param id Event ID (e.g., item ID)
   * @param score Event score/weight
   * @return Expected<void, Error> Success or error
   *
   * @throws None
   */
  utils::Expected<void, utils::Error> AddEvent(const std::string& ctx,
                                                const std::string& id,
                                                int score);

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

  std::atomic<uint64_t> total_events_{0};  ///< Total events processed
};

}  // namespace nvecd::events

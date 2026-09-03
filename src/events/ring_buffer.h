/**
 * @file ring_buffer.h
 * @brief Fixed-size circular buffer for event history
 *
 * This template class implements a thread-unsafe ring buffer that overwrites
 * the oldest elements when full. Thread safety must be provided by the caller.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace nvecd::events {

/**
 * @brief Fixed-size circular buffer with automatic overwrite
 *
 * When the buffer reaches capacity, new elements overwrite the oldest ones.
 * This class is NOT thread-safe; external synchronization is required.
 *
 * @tparam T Element type to store
 */
template <typename T>
class RingBuffer {
 public:
  /**
   * @brief Construct a ring buffer with fixed capacity
   * @param capacity Maximum number of elements to store
   */
  explicit RingBuffer(size_t capacity);

  /**
   * @brief Add an element to the buffer
   *
   * If the buffer is full, this overwrites the oldest element.
   *
   * @param item Element to add
   */
  void Push(const T& item);

  /**
   * @brief Get all elements in insertion order
   *
   * Returns elements in the order they were inserted (oldest to newest).
   *
   * @return Vector containing all elements
   */
  std::vector<T> GetAll() const;

  /**
   * @brief Visit every element in insertion order without copying
   *
   * Same traversal order as GetAll(), but the elements are passed by const
   * reference instead of being materialized into a new vector. Read-only
   * accessors that run under a lock (memory accounting, statistics) use this so
   * they neither allocate nor deep-copy the stored elements while holding it.
   *
   * @tparam Visitor Callable invoked as visitor(const T&)
   * @param visitor Callable applied to each element, oldest first
   */
  template <typename Visitor>
  void ForEach(Visitor&& visitor) const;

  /**
   * @brief Get current number of elements
   * @return Number of elements in buffer (0 to capacity)
   */
  size_t Size() const { return size_; }

  /**
   * @brief Get maximum capacity
   * @return Maximum number of elements buffer can hold
   */
  size_t Capacity() const { return capacity_; }

  /**
   * @brief Clear all elements
   */
  void Clear();

 private:
  std::vector<T> buffer_;  ///< Underlying storage
  size_t head_{0};         ///< Index where next element will be written
  size_t size_{0};         ///< Current number of elements
  size_t capacity_;        ///< Maximum capacity
};

// ============================================================================
// Template Implementation
// ============================================================================

template <typename T>
RingBuffer<T>::RingBuffer(size_t capacity) : buffer_(capacity), capacity_(capacity) {
  // Reserve capacity but don't initialize elements
}

template <typename T>
void RingBuffer<T>::Push(const T& item) {
  buffer_[head_] = item;
  head_ = (head_ + 1) % capacity_;

  if (size_ < capacity_) {
    ++size_;
  }
}

template <typename T>
std::vector<T> RingBuffer<T>::GetAll() const {
  if (size_ == 0) {
    return {};
  }

  std::vector<T> result;
  result.reserve(size_);

  if (size_ < capacity_) {
    // Buffer not yet full: elements are at [0, size_)
    for (size_t i = 0; i < size_; ++i) {
      result.push_back(buffer_[i]);
    }
  } else {
    // Buffer full: oldest element is at head_, wrap around
    for (size_t i = 0; i < capacity_; ++i) {
      size_t idx = (head_ + i) % capacity_;
      result.push_back(buffer_[idx]);
    }
  }

  return result;
}

template <typename T>
template <typename Visitor>
void RingBuffer<T>::ForEach(Visitor&& visitor) const {
  if (size_ == 0) {
    return;
  }

  if (size_ < capacity_) {
    // Buffer not yet full: elements are at [0, size_)
    for (size_t i = 0; i < size_; ++i) {
      visitor(buffer_[i]);
    }
    return;
  }

  // Buffer full: oldest element is at head_, wrap around
  for (size_t i = 0; i < capacity_; ++i) {
    const size_t idx = (head_ + i) % capacity_;
    visitor(buffer_[idx]);
  }
}

template <typename T>
void RingBuffer<T>::Clear() {
  head_ = 0;
  size_ = 0;
}

}  // namespace nvecd::events

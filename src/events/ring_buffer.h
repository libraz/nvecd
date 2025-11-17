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
  size_t head_;            ///< Index where next element will be written
  size_t size_;            ///< Current number of elements
  size_t capacity_;        ///< Maximum capacity
};

// ============================================================================
// Template Implementation
// ============================================================================

template <typename T>
RingBuffer<T>::RingBuffer(size_t capacity)
    : buffer_(capacity), head_(0), size_(0), capacity_(capacity) {
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
void RingBuffer<T>::Clear() {
  head_ = 0;
  size_ = 0;
}

}  // namespace nvecd::events

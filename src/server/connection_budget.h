/**
 * @file connection_budget.h
 * @brief Connection admission budget shared by every listener
 */

#pragma once

#include <atomic>
#include <cstddef>

namespace nvecd::server {

/**
 * @brief Fixed-size pool of connection slots
 *
 * Listeners that share one instance admit at most @p max connections in total.
 * Acquisition is a single compare-and-swap, so concurrent accept threads can
 * never hold more than the limit.
 */
class ConnectionBudget {
 public:
  explicit ConnectionBudget(size_t max) : max_(max) {}

  ConnectionBudget(const ConnectionBudget&) = delete;
  ConnectionBudget& operator=(const ConnectionBudget&) = delete;

  /// Take one slot; false when the budget is exhausted.
  bool TryAcquire() {
    size_t current = used_.load(std::memory_order_relaxed);
    while (current < max_) {
      if (used_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  /// Return a slot taken by TryAcquire().
  void Release() { used_.fetch_sub(1, std::memory_order_acq_rel); }

  size_t Used() const { return used_.load(std::memory_order_acquire); }
  size_t Max() const { return max_; }

 private:
  const size_t max_;
  std::atomic<size_t> used_{0};
};

}  // namespace nvecd::server

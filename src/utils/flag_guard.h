/**
 * @file flag_guard.h
 * @brief RAII guard for atomic boolean flags
 *
 * Reference: ../mygram-db/src/server/handlers/dump_handler.cpp
 * Reusability: 100%
 */

#pragma once

#include <atomic>

namespace nvecd::utils {

/**
 * @brief RAII guard for resetting atomic boolean flags (does NOT set on construction)
 *
 * Use after successfully acquiring the flag via compare_exchange_strong.
 * Only resets the flag to false on destruction.
 */
class FlagResetGuard {
 public:
  explicit FlagResetGuard(std::atomic<bool>& flag) : flag_(&flag) {}
  ~FlagResetGuard() {
    if (flag_ != nullptr) {
      flag_->store(false, std::memory_order_release);
    }
  }

  /** Keep the guarded flag set after destruction (fail-stop ownership). */
  void KeepSet() { flag_ = nullptr; }

  // Non-copyable and non-movable
  FlagResetGuard(const FlagResetGuard&) = delete;
  FlagResetGuard& operator=(const FlagResetGuard&) = delete;
  FlagResetGuard(FlagResetGuard&&) = delete;
  FlagResetGuard& operator=(FlagResetGuard&&) = delete;

 private:
  std::atomic<bool>* flag_;
};

}  // namespace nvecd::utils

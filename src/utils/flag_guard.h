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
 * @brief RAII guard for atomic boolean flags
 *
 * Automatically sets flag to true on construction and resets to false on
 * destruction. Ensures flag is always reset even if an error occurs.
 */
class FlagGuard {
 public:
  explicit FlagGuard(std::atomic<bool>& flag) : flag_(flag) { flag_ = true; }
  ~FlagGuard() { flag_ = false; }

  // Non-copyable and non-movable
  FlagGuard(const FlagGuard&) = delete;
  FlagGuard& operator=(const FlagGuard&) = delete;
  FlagGuard(FlagGuard&&) = delete;
  FlagGuard& operator=(FlagGuard&&) = delete;

 private:
  std::atomic<bool>& flag_;
};

/**
 * @brief RAII guard for resetting atomic boolean flags (does NOT set on construction)
 *
 * Use after successfully acquiring the flag via compare_exchange_strong.
 * Only resets the flag to false on destruction.
 */
class FlagResetGuard {
 public:
  explicit FlagResetGuard(std::atomic<bool>& flag) : flag_(flag) {}
  ~FlagResetGuard() { flag_ = false; }

  // Non-copyable and non-movable
  FlagResetGuard(const FlagResetGuard&) = delete;
  FlagResetGuard& operator=(const FlagResetGuard&) = delete;
  FlagResetGuard(FlagResetGuard&&) = delete;
  FlagResetGuard& operator=(FlagResetGuard&&) = delete;

 private:
  std::atomic<bool>& flag_;
};

}  // namespace nvecd::utils

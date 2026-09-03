/**
 * @file validated.h
 * @brief Type-level carrier for values that satisfy a store's invariants
 */

#pragma once

#include <utility>

namespace nvecd::events {

/**
 * @brief A value that is known to satisfy the invariants checked by @p Minter
 *
 * The constructor is private and reachable only from @p Minter, so a raw value
 * cannot be handed to a store mutation path without first passing through the
 * matching validator. Live ingestion and snapshot/WAL restore mint through the
 * same validator, which is what keeps the set of inputs the two paths accept
 * identical: restore preserves score values, types and timestamps verbatim, but
 * it is not exempt from the invariants.
 *
 * @tparam T Wrapped value type
 * @tparam Minter Validator type permitted to construct instances
 */
template <typename T, typename Minter>
class Validated {
 public:
  /** @brief Access the wrapped, already validated value. */
  const T& Get() const noexcept { return value_; }

  /** @brief Access the wrapped, already validated value. */
  const T& operator*() const noexcept { return value_; }

  /** @brief Access members of the wrapped, already validated value. */
  const T* operator->() const noexcept { return &value_; }

 private:
  friend Minter;

  explicit Validated(T value) : value_(std::move(value)) {}

  T value_;
};

}  // namespace nvecd::events

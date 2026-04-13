/**
 * @file metadata_filter.cpp
 * @brief Metadata filter implementation
 */

#include "vectors/metadata_filter.h"

#include <algorithm>

namespace nvecd::vectors {

namespace {

/// Compare two MetadataValue instances using < ordering.
/// Returns -1, 0, or 1 (like strcmp).
/// Only meaningful when both are the same type and numeric/string.
int CompareValues(const MetadataValue& lhs, const MetadataValue& rhs) {
  // Type mismatch: try numeric promotion (int64 vs double)
  if (lhs.index() != rhs.index()) {
    // int64 vs double
    if (std::holds_alternative<int64_t>(lhs) && std::holds_alternative<double>(rhs)) {
      auto l = static_cast<double>(std::get<int64_t>(lhs));
      double r = std::get<double>(rhs);
      if (l < r)
        return -1;
      if (l > r)
        return 1;
      return 0;
    }
    if (std::holds_alternative<double>(lhs) && std::holds_alternative<int64_t>(rhs)) {
      double l = std::get<double>(lhs);
      auto r = static_cast<double>(std::get<int64_t>(rhs));
      if (l < r)
        return -1;
      if (l > r)
        return 1;
      return 0;
    }
    // Other type mismatches: not comparable
    return -2;  // Sentinel for "not comparable"
  }

  return std::visit(
      [](const auto& l, const auto& r) -> int {
        using L = std::decay_t<decltype(l)>;
        using R = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<L, R>) {
          if (l < r)
            return -1;
          if (r < l)
            return 1;
          return 0;
        }
        return -2;  // Different types (shouldn't reach here due to index check)
      },
      lhs, rhs);
}

}  // namespace

bool FilterCondition::Match(const MetadataValue& meta_value) const {
  switch (op) {
    case FilterOp::kEq: {
      int cmp = CompareValues(meta_value, value);
      return cmp == 0;
    }
    case FilterOp::kNe: {
      int cmp = CompareValues(meta_value, value);
      return cmp != 0 && cmp != -2;  // Not equal and comparable
    }
    case FilterOp::kGt: {
      int cmp = CompareValues(meta_value, value);
      return cmp == 1;
    }
    case FilterOp::kGe: {
      int cmp = CompareValues(meta_value, value);
      return cmp >= 0;
    }
    case FilterOp::kLt: {
      int cmp = CompareValues(meta_value, value);
      return cmp == -1;
    }
    case FilterOp::kLe: {
      int cmp = CompareValues(meta_value, value);
      return cmp == 0 || cmp == -1;
    }
    case FilterOp::kIn: {
      return std::any_of(values.begin(), values.end(),
                         [&meta_value](const MetadataValue& v) { return CompareValues(meta_value, v) == 0; });
    }
  }
  return false;
}

bool MetadataFilter::Match(const Metadata& meta) const {
  for (const auto& condition : conditions) {
    auto it = meta.find(condition.field);
    if (it == meta.end()) {
      // Field not present: condition fails (unless kNe)
      if (condition.op == FilterOp::kNe) {
        continue;  // Missing field != value is true
      }
      return false;
    }
    if (!condition.Match(it->second)) {
      return false;
    }
  }
  return true;
}

}  // namespace nvecd::vectors

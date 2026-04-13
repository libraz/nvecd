/**
 * @file metadata_filter.h
 * @brief Metadata filter conditions for attribute-based search filtering
 *
 * Provides FilterCondition and MetadataFilter for post-filter and
 * pre-filter operations on metadata. Phase 1 supports AND-only logic.
 */

#pragma once

#include <string>
#include <vector>

#include "vectors/metadata.h"

namespace nvecd::vectors {

/// Filter comparison operators
enum class FilterOp {
  kEq,  ///< field == value
  kNe,  ///< field != value
  kGt,  ///< field > value
  kGe,  ///< field >= value
  kLt,  ///< field < value
  kLe,  ///< field <= value
  kIn,  ///< field IN (values...)
};

/**
 * @brief A single filter condition
 */
struct FilterCondition {
  std::string field;                  ///< Metadata field name
  FilterOp op;                        ///< Comparison operator
  MetadataValue value;                ///< Value for Eq, Ne, Gt, Ge, Lt, Le
  std::vector<MetadataValue> values;  ///< Values for In operator

  /**
   * @brief Check if a single metadata value matches this condition
   * @param meta_value The value from metadata
   * @return True if the condition is satisfied
   */
  bool Match(const MetadataValue& meta_value) const;
};

/**
 * @brief AND-combined metadata filter (all conditions must match)
 */
struct MetadataFilter {
  std::vector<FilterCondition> conditions;

  /**
   * @brief Check if metadata matches all conditions
   * @param meta The item's metadata
   * @return True if all conditions are satisfied
   */
  bool Match(const Metadata& meta) const;

  /**
   * @brief Check if the filter has any conditions
   */
  bool Empty() const { return conditions.empty(); }
};

}  // namespace nvecd::vectors

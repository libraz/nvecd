/**
 * @file metadata.h
 * @brief Metadata types for vector items
 *
 * Defines the metadata value type (variant of string, int64, double, bool)
 * and per-item metadata as a string-keyed map.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>

namespace nvecd::vectors {

/// Metadata value: string, int64, double, or bool
using MetadataValue = std::variant<std::string, int64_t, double, bool>;

/// Per-item metadata: field_name -> value
using Metadata = std::unordered_map<std::string, MetadataValue>;

}  // namespace nvecd::vectors

/**
 * @file string_utils.h
 * @brief String formatting and secret comparison utilities
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace nvecd::utils {

/**
 * @brief Format bytes to human-readable string (e.g., "1.5MB", "500KB")
 *
 * @param bytes Number of bytes
 * @return Human-readable string
 */
std::string FormatBytes(size_t bytes);

/**
 * @brief Compare two opaque secrets without revealing the matching prefix length.
 */
bool ConstantTimeEquals(std::string_view left, std::string_view right);

}  // namespace nvecd::utils

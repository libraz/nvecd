/**
 * @file string_utils.cpp
 * @brief Implementation of string utility functions
 */

#include "utils/string_utils.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

namespace nvecd::utils {

namespace {

// Byte formatting constants
constexpr double kBytesPerKilobyte = 1024.0;
constexpr double kLargeUnitThreshold = 100.0;
constexpr double kMediumUnitThreshold = 10.0;

}  // namespace

std::string FormatBytes(size_t bytes) {
  constexpr std::array<const char*, 5> kUnits = {"B", "KB", "MB", "GB", "TB"};

  if (bytes == 0) {
    return "0B";
  }

  size_t unit_index = 0;
  auto size = static_cast<double>(bytes);

  while (size >= kBytesPerKilobyte && unit_index < kUnits.size() - 1) {
    size /= kBytesPerKilobyte;
    unit_index++;
  }

  // Format with appropriate precision
  std::ostringstream oss;
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
  if (size >= kLargeUnitThreshold) {
    oss << std::fixed << std::setprecision(0) << size << kUnits[unit_index];
  } else if (size >= kMediumUnitThreshold) {
    oss << std::fixed << std::setprecision(1) << size << kUnits[unit_index];
  } else {
    oss << std::fixed << std::setprecision(2) << size << kUnits[unit_index];
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

  return oss.str();
}

bool ConstantTimeEquals(std::string_view left, std::string_view right) {
  const size_t max_size = std::max(left.size(), right.size());
  size_t difference = left.size() ^ right.size();
  for (size_t i = 0; i < max_size; ++i) {
    const unsigned char left_byte = i < left.size() ? static_cast<unsigned char>(left[i]) : 0;
    const unsigned char right_byte = i < right.size() ? static_cast<unsigned char>(right[i]) : 0;
    difference |= static_cast<size_t>(left_byte ^ right_byte);
  }
  return difference == 0;
}

}  // namespace nvecd::utils

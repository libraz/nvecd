/**
 * @file version.h
 * @brief nvecd version information
 */

#pragma once

#include <string>

namespace nvecd {

/**
 * @brief Version information
 */
class Version {
 public:
  /**
   * @brief Get version string
   * @return Version string (e.g., "0.1.0")
   */
  static std::string String() { return "0.1.0"; }

  /**
   * @brief Get major version
   */
  static int Major() { return 0; }

  /**
   * @brief Get minor version
   */
  static int Minor() { return 1; }

  /**
   * @brief Get patch version
   */
  static int Patch() { return 0; }
};

}  // namespace nvecd

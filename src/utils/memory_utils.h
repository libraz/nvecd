/**
 * @file memory_utils.h
 * @brief Memory health check and monitoring utilities
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace nvecd::utils {

/**
 * @brief System memory information
 */
struct SystemMemoryInfo {
  uint64_t total_physical_bytes;      ///< Total physical RAM
  uint64_t available_physical_bytes;  ///< Available physical RAM
  uint64_t total_swap_bytes;          ///< Total swap space
  uint64_t available_swap_bytes;      ///< Available swap space
};

/**
 * @brief Process memory usage information
 */
struct ProcessMemoryInfo {
  uint64_t rss_bytes;       ///< Resident Set Size (physical memory used)
  uint64_t virtual_bytes;   ///< Virtual memory size
  uint64_t peak_rss_bytes;  ///< Peak RSS (high water mark)
};

namespace detail {
SystemMemoryInfo ApplyCgroupLimit(SystemMemoryInfo host, uint64_t limit_bytes, uint64_t usage_bytes);
}

/**
 * @brief Memory health status
 */
enum class MemoryHealthStatus : std::uint8_t {
  HEALTHY,   ///< Sufficient memory available (>20%)
  WARNING,   ///< Memory running low (10-20%)
  CRITICAL,  ///< Memory critically low (<10%)
  UNKNOWN    ///< Unable to determine status
};

/**
 * @brief Get system memory information
 *
 * @return System memory info, or std::nullopt on error
 */
std::optional<SystemMemoryInfo> GetSystemMemoryInfo();

/**
 * @brief Get current process memory usage
 *
 * @return Process memory info, or std::nullopt on error
 */
std::optional<ProcessMemoryInfo> GetProcessMemoryInfo();

/**
 * @brief Get current memory health status
 *
 * @return Memory health status enum
 */
MemoryHealthStatus GetMemoryHealthStatus();

/** Classify one memory sample without probing the system again. */
MemoryHealthStatus GetMemoryHealthStatus(const SystemMemoryInfo& info);

/**
 * @brief Get human-readable string for memory health status
 *
 * @param status Memory health status
 * @return String representation ("HEALTHY", "WARNING", "CRITICAL", "UNKNOWN")
 */
std::string MemoryHealthStatusToString(MemoryHealthStatus status);

}  // namespace nvecd::utils

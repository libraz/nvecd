/**
 * @file memory_utils.cpp
 * @brief Memory health check and monitoring utilities implementation
 */

#include "utils/memory_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#elif __linux__
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

namespace nvecd::utils {

namespace {

// Safety thresholds for memory health status
constexpr double kHealthyThreshold = 0.2;  // 20% available = healthy
constexpr double kWarningThreshold = 0.1;  // 10% available = warning
// Below 10% = critical

#ifdef __linux__
std::optional<uint64_t> ReadCgroupCounter(const char* path) {
  std::ifstream input(path);
  std::string value;
  if (!(input >> value) || value == "max") {
    return std::nullopt;
  }
  try {
    size_t consumed = 0;
    const uint64_t parsed = std::stoull(value, &consumed);
    return consumed == value.size() ? std::optional<uint64_t>(parsed) : std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}
#endif
}  // namespace

SystemMemoryInfo detail::ApplyCgroupLimit(SystemMemoryInfo host, uint64_t limit_bytes, uint64_t usage_bytes) {
  if (limit_bytes == 0 || limit_bytes >= host.total_physical_bytes) {
    return host;
  }
  host.total_physical_bytes = limit_bytes;
  const uint64_t cgroup_available = usage_bytes >= limit_bytes ? 0 : limit_bytes - usage_bytes;
  host.available_physical_bytes = std::min(host.available_physical_bytes, cgroup_available);
  return host;
}

std::optional<SystemMemoryInfo> GetSystemMemoryInfo() {
  SystemMemoryInfo info{};

#ifdef __APPLE__
  // Get total physical memory
  // C-style array required by macOS sysctl() API
  int mib[2] = {CTL_HW, HW_MEMSIZE};  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  uint64_t physical_memory = 0;
  size_t length = sizeof(physical_memory);
  // Array decay required by macOS sysctl() system call
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  if (sysctl(mib, 2, &physical_memory, &length, nullptr, 0) != 0) {
    spdlog::error("Failed to get total physical memory (sysctl)");
    return std::nullopt;
  }
  info.total_physical_bytes = physical_memory;

  // Get VM statistics for available memory
  mach_port_t host_port = mach_host_self();
  vm_size_t page_size = 0;
  if (host_page_size(host_port, &page_size) != KERN_SUCCESS) {
    mach_port_deallocate(mach_task_self(), host_port);
    return std::nullopt;
  }

  vm_statistics64_data_t vm_stats{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  // kern_return_t is standard Mach API naming; reinterpret_cast required by Mach API
  // NOLINTNEXTLINE(readability-identifier-length)
  kern_return_t kern_ret = host_statistics64(host_port, HOST_VM_INFO64,
                                             // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                                             reinterpret_cast<host_info64_t>(&vm_stats), &count);

  mach_port_deallocate(mach_task_self(), host_port);

  if (kern_ret != KERN_SUCCESS) {
    spdlog::error("Failed to get VM statistics");
    return std::nullopt;
  }

  // Available = free + inactive pages
  uint64_t free_pages = vm_stats.free_count;
  uint64_t inactive_pages = vm_stats.inactive_count;
  info.available_physical_bytes = (free_pages + inactive_pages) * page_size;

  // macOS swap info (from swapusage sysctl)
  struct xsw_usage swap_info {};
  size_t swap_size = sizeof(swap_info);
  // C-style array required by macOS sysctl() API
  int swap_mib[2] = {CTL_VM, VM_SWAPUSAGE};  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  // Array decay required by macOS sysctl() system call
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  if (sysctl(swap_mib, 2, &swap_info, &swap_size, nullptr, 0) == 0) {
    info.total_swap_bytes = swap_info.xsu_total;
    info.available_swap_bytes = swap_info.xsu_avail;
  } else {
    info.total_swap_bytes = 0;
    info.available_swap_bytes = 0;
  }

#elif __linux__
  // Read /proc/meminfo for detailed memory information
  constexpr uint64_t kBytesPerKB = 1024ULL;
  std::ifstream meminfo("/proc/meminfo");
  if (!meminfo) {
    spdlog::error("Failed to open /proc/meminfo");
    return std::nullopt;
  }

  std::string line;
  while (std::getline(meminfo, line)) {
    std::istringstream iss(line);
    std::string key;
    uint64_t value = 0;
    std::string unit;

    iss >> key >> value >> unit;

    // Convert kB to bytes
    value *= kBytesPerKB;

    if (key == "MemTotal:") {
      info.total_physical_bytes = value;
    } else if (key == "MemAvailable:") {
      info.available_physical_bytes = value;
    } else if (key == "SwapTotal:") {
      info.total_swap_bytes = value;
    } else if (key == "SwapFree:") {
      info.available_swap_bytes = value;
    }
  }

  // Validate we got the essential information
  if (info.total_physical_bytes == 0) {
    spdlog::error("Failed to parse total physical memory from /proc/meminfo");
    return std::nullopt;
  }

  auto cgroup_limit = ReadCgroupCounter("/sys/fs/cgroup/memory.max");
  auto cgroup_usage = ReadCgroupCounter("/sys/fs/cgroup/memory.current");
  if (!cgroup_limit.has_value() || !cgroup_usage.has_value()) {
    cgroup_limit = ReadCgroupCounter("/sys/fs/cgroup/memory/memory.limit_in_bytes");
    cgroup_usage = ReadCgroupCounter("/sys/fs/cgroup/memory/memory.usage_in_bytes");
  }
  if (cgroup_limit.has_value() && cgroup_usage.has_value() && *cgroup_limit != std::numeric_limits<uint64_t>::max()) {
    info = detail::ApplyCgroupLimit(info, *cgroup_limit, *cgroup_usage);
  }

#else
  spdlog::error("Unsupported platform for memory info");
  return std::nullopt;
#endif

  return info;
}

std::optional<ProcessMemoryInfo> GetProcessMemoryInfo() {
  ProcessMemoryInfo info{};

#ifdef __APPLE__
  // Get task info for current process
  struct task_basic_info_64 task_basic_info {};
  mach_msg_type_number_t count = TASK_BASIC_INFO_64_COUNT;
  // kern_return_t is standard Mach API naming; reinterpret_cast required by Mach task_info() API
  // NOLINTNEXTLINE(readability-identifier-length,cppcoreguidelines-pro-type-reinterpret-cast)
  kern_return_t kern_ret =
      task_info(mach_task_self(), TASK_BASIC_INFO_64,
                reinterpret_cast<task_info_t>(&task_basic_info),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                &count);

  if (kern_ret != KERN_SUCCESS) {
    spdlog::error("Failed to get task info");
    return std::nullopt;
  }

  info.rss_bytes = task_basic_info.resident_size;
  info.virtual_bytes = task_basic_info.virtual_size;

  // Peak RSS from rusage
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    // ru_maxrss is in bytes on macOS
    info.peak_rss_bytes = static_cast<uint64_t>(usage.ru_maxrss);
  } else {
    info.peak_rss_bytes = info.rss_bytes;
  }

#elif __linux__
  // Read /proc/self/status for memory info
  constexpr uint64_t kBytesPerKB = 1024ULL;
  std::ifstream status("/proc/self/status");
  if (!status) {
    spdlog::error("Failed to open /proc/self/status");
    return std::nullopt;
  }

  std::string line;
  while (std::getline(status, line)) {
    std::istringstream iss(line);
    std::string key;
    uint64_t value = 0;
    std::string unit;

    iss >> key >> value >> unit;

    // Convert kB to bytes
    value *= kBytesPerKB;

    if (key == "VmRSS:") {
      info.rss_bytes = value;
    } else if (key == "VmSize:") {
      info.virtual_bytes = value;
    } else if (key == "VmHWM:") {
      info.peak_rss_bytes = value;
    }
  }

  // Validate we got essential information
  if (info.rss_bytes == 0) {
    spdlog::error("Failed to parse RSS from /proc/self/status");
    return std::nullopt;
  }

#else
  spdlog::error("Unsupported platform for process memory info");
  return std::nullopt;
#endif

  return info;
}

MemoryHealthStatus GetMemoryHealthStatus() {
  auto system_info = GetSystemMemoryInfo();
  if (!system_info) {
    return MemoryHealthStatus::UNKNOWN;
  }

  return GetMemoryHealthStatus(*system_info);
}

MemoryHealthStatus GetMemoryHealthStatus(const SystemMemoryInfo& info) {
  if (info.total_physical_bytes == 0) {
    return MemoryHealthStatus::UNKNOWN;
  }
  const double available_ratio =
      static_cast<double>(info.available_physical_bytes) / static_cast<double>(info.total_physical_bytes);

  if (available_ratio >= kHealthyThreshold) {
    return MemoryHealthStatus::HEALTHY;
  }
  if (available_ratio >= kWarningThreshold) {
    return MemoryHealthStatus::WARNING;
  }
  return MemoryHealthStatus::CRITICAL;
}

std::string MemoryHealthStatusToString(MemoryHealthStatus status) {
  switch (status) {
    case MemoryHealthStatus::HEALTHY:
      return "HEALTHY";
    case MemoryHealthStatus::WARNING:
      return "WARNING";
    case MemoryHealthStatus::CRITICAL:
      return "CRITICAL";
    case MemoryHealthStatus::UNKNOWN:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

}  // namespace nvecd::utils

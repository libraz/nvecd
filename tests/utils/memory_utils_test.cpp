#include "utils/memory_utils.h"

#include <gtest/gtest.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace nvecd::utils {
namespace {

TEST(MemoryUtilsTest, HealthUsesOneSampleAndExactThresholds) {
  EXPECT_EQ(GetMemoryHealthStatus({100, 20, 0, 0}), MemoryHealthStatus::HEALTHY);
  EXPECT_EQ(GetMemoryHealthStatus({100, 19, 0, 0}), MemoryHealthStatus::WARNING);
  EXPECT_EQ(GetMemoryHealthStatus({100, 10, 0, 0}), MemoryHealthStatus::WARNING);
  EXPECT_EQ(GetMemoryHealthStatus({100, 9, 0, 0}), MemoryHealthStatus::CRITICAL);
  EXPECT_EQ(GetMemoryHealthStatus({0, 0, 0, 0}), MemoryHealthStatus::UNKNOWN);
}

#ifdef __APPLE__
TEST(MemoryUtilsTest, SystemProbeDoesNotLeakHostSendRights) {
  const mach_port_t host = mach_host_self();
  mach_port_urefs_t before = 0;
  ASSERT_EQ(mach_port_get_refs(mach_task_self(), host, MACH_PORT_RIGHT_SEND, &before), KERN_SUCCESS);
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(GetSystemMemoryInfo().has_value());
  }
  mach_port_urefs_t after = 0;
  ASSERT_EQ(mach_port_get_refs(mach_task_self(), host, MACH_PORT_RIGHT_SEND, &after), KERN_SUCCESS);
  EXPECT_EQ(after, before);
  EXPECT_EQ(mach_port_deallocate(mach_task_self(), host), KERN_SUCCESS);
}
#endif

TEST(MemoryUtilsTest, CgroupLimitOverridesLargerHostCapacity) {
  SystemMemoryInfo host{16'000, 8'000, 0, 0};
  const auto effective = detail::ApplyCgroupLimit(host, 2'000, 1'500);
  EXPECT_EQ(effective.total_physical_bytes, 2'000U);
  EXPECT_EQ(effective.available_physical_bytes, 500U);
}

TEST(MemoryUtilsTest, HostAvailabilityRemainsTheTighterLimit) {
  SystemMemoryInfo host{16'000, 200, 0, 0};
  const auto effective = detail::ApplyCgroupLimit(host, 2'000, 1'000);
  EXPECT_EQ(effective.available_physical_bytes, 200U);
}

TEST(MemoryUtilsTest, UnlimitedOrLargerCgroupDoesNotOverrideHost) {
  const SystemMemoryInfo host{2'000, 500, 0, 0};
  EXPECT_EQ(detail::ApplyCgroupLimit(host, 0, 0).total_physical_bytes, 2'000U);
  EXPECT_EQ(detail::ApplyCgroupLimit(host, 4'000, 3'000).total_physical_bytes, 2'000U);
}

}  // namespace
}  // namespace nvecd::utils

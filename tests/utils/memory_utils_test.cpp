#include "utils/memory_utils.h"

#include <gtest/gtest.h>

namespace nvecd::utils {
namespace {

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

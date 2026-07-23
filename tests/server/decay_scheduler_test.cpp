#include "server/decay_scheduler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace nvecd::server {
namespace {

TEST(DecaySchedulerTest, RoutesMaintenanceThroughDurabilityCallbackBeforeApply) {
  events::CoOccurrenceIndex index;
  index.SetScore("item1", "item2", 100.0F);
  std::atomic<int> callbacks{0};

  DecayScheduler scheduler(&index, 1, 0.5, [&](double alpha, bool prune) -> utils::Expected<void, utils::Error> {
    EXPECT_FALSE(prune);
    callbacks.fetch_add(1, std::memory_order_relaxed);
    index.ApplyDecay(alpha);
    return {};
  });
  scheduler.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(1300));
  scheduler.Stop();

  EXPECT_EQ(callbacks.load(std::memory_order_relaxed), 1);
  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 50.0F);
}

TEST(DecaySchedulerTest, FailedDurabilityCallbackDoesNotApplyMaintenance) {
  events::CoOccurrenceIndex index;
  index.SetScore("item1", "item2", 100.0F);

  DecayScheduler scheduler(&index, 1, 0.5, [](double, bool) -> utils::Expected<void, utils::Error> {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kEventDecayFailed, "injected persistence failure"));
  });
  scheduler.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(1300));
  scheduler.Stop();

  EXPECT_FLOAT_EQ(index.GetScore("item1", "item2"), 100.0F);
}

}  // namespace
}  // namespace nvecd::server

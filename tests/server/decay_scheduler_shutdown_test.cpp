#include <gtest/gtest.h>

#include <chrono>

#include "events/co_occurrence_index.h"
#include "server/decay_scheduler.h"

namespace nvecd::server {
namespace {

TEST(DecaySchedulerShutdownTest, StopWakesTheWorkerInsteadOfWaitingOutTheInterval) {
  events::CoOccurrenceIndex index;
  DecayScheduler scheduler(&index, 10, 0.5);
  scheduler.Start();
  ASSERT_TRUE(scheduler.IsRunning());

  const auto begin = std::chrono::steady_clock::now();
  scheduler.Stop();

  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(200));
  EXPECT_FALSE(scheduler.IsRunning());
}

}  // namespace
}  // namespace nvecd::server

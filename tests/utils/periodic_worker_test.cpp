#include "utils/periodic_worker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace nvecd::utils {
namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

TEST(PeriodicWorkerTest, StopReturnsWithoutWaitingOutThePeriod) {
  std::atomic<int> ticks{0};
  PeriodicWorker worker;
  worker.Start(std::chrono::seconds(10), [&]() { ticks.fetch_add(1); });

  const auto begin = steady_clock::now();
  worker.Stop();

  EXPECT_LT(steady_clock::now() - begin, milliseconds(200));
  EXPECT_EQ(ticks.load(), 0);
}

TEST(PeriodicWorkerTest, TicksRepeatedlyUntilStopped) {
  std::atomic<int> ticks{0};
  PeriodicWorker worker;
  worker.Start(milliseconds(10), [&]() { ticks.fetch_add(1); });
  std::this_thread::sleep_for(milliseconds(150));
  worker.Stop();

  const int after_stop = ticks.load();
  EXPECT_GE(after_stop, 2);
  std::this_thread::sleep_for(milliseconds(50));
  EXPECT_EQ(ticks.load(), after_stop);
}

TEST(PeriodicWorkerTest, StopIsIdempotentAndWorkerCanRestart) {
  std::atomic<int> ticks{0};
  PeriodicWorker worker;
  worker.Stop();
  worker.Start(milliseconds(5), [&]() { ticks.fetch_add(1); });
  worker.Stop();
  worker.Stop();

  const int first_run = ticks.load();
  worker.Start(milliseconds(5), [&]() { ticks.fetch_add(1); });
  std::this_thread::sleep_for(milliseconds(100));
  worker.Stop();
  EXPECT_GT(ticks.load(), first_run);
}

}  // namespace
}  // namespace nvecd::utils

/**
 * @file rate_limiter_test.cpp
 * @brief Unit tests for RateLimiter (token bucket per-client rate limiting)
 */

#include "server/rate_limiter.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

using nvecd::server::RateLimiter;

// ============================================================================
// Basic functionality tests
// ============================================================================

TEST(RateLimiterTest, AllowUpToCapacity) {
  // Capacity=10, refill_rate=0 (no refill), max_clients=100
  RateLimiter limiter(10, 0, 100);
  const std::string client = "192.168.1.1";

  // First call creates bucket with capacity-1 tokens remaining, so 10 total
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(limiter.Allow(client)) << "Request " << i << " should be allowed";
  }
}

TEST(RateLimiterTest, RejectAfterExhausted) {
  RateLimiter limiter(5, 0, 100);
  const std::string client = "192.168.1.2";

  // Exhaust all 5 tokens
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(limiter.Allow(client));
  }

  // Next call should be rejected
  EXPECT_FALSE(limiter.Allow(client));
  EXPECT_FALSE(limiter.Allow(client));
}

TEST(RateLimiterTest, RefillOverTime) {
  // Capacity=5, refill_rate=50 tokens/sec (so 200ms ~ 10 tokens refilled, capped at 5)
  RateLimiter limiter(5, 50, 100);
  const std::string client = "192.168.1.3";

  // Exhaust all tokens
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(limiter.Allow(client));
  }
  EXPECT_FALSE(limiter.Allow(client));

  // Sleep to allow refill
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Tokens should have refilled (50 tokens/sec * 0.2s = 10, capped at capacity 5)
  EXPECT_TRUE(limiter.Allow(client));
}

TEST(RateLimiterTest, IndependentPerClient) {
  RateLimiter limiter(3, 0, 100);
  const std::string client_a = "client_a";
  const std::string client_b = "client_b";

  // Exhaust client A
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(limiter.Allow(client_a));
  }
  EXPECT_FALSE(limiter.Allow(client_a));

  // Client B should still have full capacity
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(limiter.Allow(client_b)) << "Client B request " << i << " should be allowed";
  }
  EXPECT_FALSE(limiter.Allow(client_b));
}

TEST(RateLimiterTest, ConcurrentAccess) {
  // Verify no crashes under concurrent access
  RateLimiter limiter(1000, 100, 100);
  constexpr int kNumThreads = 8;
  constexpr int kRequestsPerThread = 500;

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);

  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&limiter, t]() {
      const std::string key = "client_" + std::to_string(t % 4);
      for (int i = 0; i < kRequestsPerThread; ++i) {
        // Just call Allow; we only care that it doesn't crash
        limiter.Allow(key);
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  // If we get here without crash or deadlock, the test passes
  SUCCEED();
}

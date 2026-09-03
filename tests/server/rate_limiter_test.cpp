/**
 * @file rate_limiter_test.cpp
 * @brief Unit tests for RateLimiter (token bucket per-client rate limiting)
 */

#include "server/rate_limiter.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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

TEST(RateLimiterTest, ConcurrentAccessNeverGrantsMoreThanTheBucketCouldHold) {
  // Configuration validation rejects api.rate_limiting.refill_rate <= 0, so the
  // concurrent case is pinned with a rate an operator can actually run. The
  // wall clock is measured rather than assumed: the upper bound is the burst
  // plus every token the refill could conceivably have added over the observed
  // run time, which no amount of scheduling delay can violate.
  constexpr int kCapacity = 100;
  constexpr int kRefillRate = 1;
  constexpr int kNumKeys = 4;
  constexpr int kNumThreads = 8;
  constexpr int kRequestsPerThread = 500;
  constexpr int kRequestsPerKey = kNumThreads * kRequestsPerThread / kNumKeys;
  static_assert(kRequestsPerKey > kCapacity, "each key must be offered more requests than its bucket can grant");

  RateLimiter limiter(kCapacity, kRefillRate, /*max_clients=*/64);

  std::array<std::atomic<int>, kNumKeys> granted{};
  for (auto& counter : granted) {
    counter.store(0, std::memory_order_relaxed);
  }

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  const auto started = std::chrono::steady_clock::now();
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&limiter, &granted, t]() {
      const int key_index = t % kNumKeys;
      const std::string key = "client_" + std::to_string(key_index);
      for (int i = 0; i < kRequestsPerThread; ++i) {
        if (limiter.Allow(key)) {
          granted[key_index].fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }
  const double elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  // Deliberate over-estimate: the window is timed from before the threads even
  // start, so it is longer than any bucket was actually alive.
  const int max_grantable = kCapacity + static_cast<int>(std::ceil(elapsed_sec * kRefillRate));
  ASSERT_LT(max_grantable, kRequestsPerKey)
      << "the run took long enough that refill could have covered the offered load, so the bound proves nothing";

  for (int key_index = 0; key_index < kNumKeys; ++key_index) {
    const int count = granted[key_index].load(std::memory_order_relaxed);
    // The bucket starts full and is offered far more requests than it holds,
    // so a limiter that grants fewer than its burst has lost a token.
    EXPECT_GE(count, kCapacity) << "client_" << key_index << " granted fewer requests than its burst";
    EXPECT_LE(count, max_grantable) << "client_" << key_index << " granted more requests than it could hold";
  }
}

// ============================================================================
// Client tracking: LRU eviction and the max_clients ceiling
//
// These cases construct the limiter with refill_rate = 0. Configuration
// validation rejects that value for api.rate_limiting.refill_rate, so no
// running server is in this state, but RateLimiter is a token bucket whose own
// contract accepts it and this is the only way to keep an exhausted bucket
// exhausted for the length of a test without a wall-clock dependency. That is
// what makes eviction observable at all: the entire eviction signal is "this
// key's bucket came back full". Do not raise it to 1 to match the schema; the
// concurrent case above already covers a loadable refill rate.
// ============================================================================

TEST(RateLimiterTest, EvictsLeastRecentlyUsedClientWhenTrackingIsFull) {
  RateLimiter limiter(/*capacity=*/2, /*refill_rate=*/0, /*max_clients=*/2);

  EXPECT_TRUE(limiter.Allow("a"));  // a: 1 token left
  EXPECT_TRUE(limiter.Allow("b"));  // b: 1 token left
  EXPECT_TRUE(limiter.Allow("a"));  // a: 0 tokens left, a is now most recent

  // Tracking is full, so admitting c evicts the least recently used key (b).
  EXPECT_TRUE(limiter.Allow("c"));

  // a was not the eviction victim: its exhausted bucket is still tracked.
  EXPECT_FALSE(limiter.Allow("a"));

  // b was evicted, so it is admitted as a brand new client with a full bucket.
  // Had its bucket survived it would have had only one token left.
  EXPECT_TRUE(limiter.Allow("b"));
  EXPECT_TRUE(limiter.Allow("b"));
  EXPECT_FALSE(limiter.Allow("b"));
}

TEST(RateLimiterTest, DeniedRequestStillPromotesItsClientInLruOrder) {
  RateLimiter limiter(/*capacity=*/1, /*refill_rate=*/0, /*max_clients=*/2);

  EXPECT_TRUE(limiter.Allow("a"));  // a: 0 tokens left
  EXPECT_TRUE(limiter.Allow("b"));  // b: 0 tokens left, a is least recent

  // The LRU position is updated before the token check, so even a rate-limited
  // request marks its client as recently seen.
  EXPECT_FALSE(limiter.Allow("a"));

  // b is now the least recently used key and is the one evicted for c.
  EXPECT_TRUE(limiter.Allow("c"));
  EXPECT_FALSE(limiter.Allow("a")) << "a should have survived eviction after its denied request refreshed it";
  EXPECT_TRUE(limiter.Allow("b")) << "b should have been evicted and readmitted with a fresh bucket";
}

TEST(RateLimiterTest, MaxClientsCeilingThrashesBucketsForAlternatingClients) {
  // A max_clients ceiling below the number of active clients keeps only that
  // many buckets alive; the evicted client is readmitted with a full bucket, so
  // alternating clients are never throttled. This is the documented cost of
  // sizing max_clients too small.
  RateLimiter limiter(/*capacity=*/1, /*refill_rate=*/0, /*max_clients=*/1);

  for (int round = 0; round < 5; ++round) {
    EXPECT_TRUE(limiter.Allow("x")) << "round " << round;
    EXPECT_TRUE(limiter.Allow("y")) << "round " << round;
  }

  // Within the ceiling, the surviving client is still limited normally.
  RateLimiter roomy(/*capacity=*/1, /*refill_rate=*/0, /*max_clients=*/2);
  EXPECT_TRUE(roomy.Allow("x"));
  EXPECT_TRUE(roomy.Allow("y"));
  EXPECT_FALSE(roomy.Allow("x"));
  EXPECT_FALSE(roomy.Allow("y"));
}

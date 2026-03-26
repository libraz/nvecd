/**
 * @file tiktok_1m_scale_test.cpp
 * @brief TikTok-like scenario benchmarks with large pre-loaded datasets up to 1M items
 *
 * Benchmarks system behavior at scale (10K to 1M vectors) measuring query
 * latency degradation, memory usage, and throughput under concurrent load.
 * Uses parallel bulk loading to efficiently populate large datasets.
 *
 * All tests are DISABLED by default (CI-safe). Run with:
 *   --gtest_also_run_disabled_tests --gtest_filter="TikTok1MScaleTest.*"
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../test_server_fixture.h"
#include "../test_tcp_client.h"

namespace {

/// Number of content categories for the video catalog.
constexpr size_t kNumCategories = 10;

/// Vector dimension used across all tests.
constexpr int kVectorDimension = 32;

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

/**
 * @brief Measure execution time and return duration in milliseconds
 */
template <typename Func>
double MeasureMs(Func&& func) {
  auto start = std::chrono::high_resolution_clock::now();
  func();
  auto end = std::chrono::high_resolution_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

/**
 * @brief Compute percentile from sorted durations
 */
double Percentile(std::vector<double>& sorted_values, double p) {
  if (sorted_values.empty()) {
    return 0.0;
  }
  size_t idx = static_cast<size_t>(
      p / 100.0 * static_cast<double>(sorted_values.size() - 1));
  return sorted_values[idx];
}

/**
 * @brief Print performance report header
 */
void PrintHeader(const std::string& test_name) {
  std::cout << "\n========================================\n";
  std::cout << "  " << test_name << "\n";
  std::cout << "========================================\n";
}

/**
 * @brief Print throughput metrics
 */
void PrintThroughput(const std::string& label, int ops, double duration_ms) {
  double ops_per_sec =
      (duration_ms > 0)
          ? (static_cast<double>(ops) / duration_ms * 1000.0)
          : 0.0;
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "  " << label << ": " << ops << " ops in " << duration_ms
            << " ms (" << ops_per_sec << " ops/sec)\n";
}

/**
 * @brief Print latency percentiles
 */
void PrintLatency(const std::string& label, std::vector<double>& latencies_ms) {
  if (latencies_ms.empty()) {
    return;
  }
  std::sort(latencies_ms.begin(), latencies_ms.end());
  double avg = std::accumulate(latencies_ms.begin(), latencies_ms.end(), 0.0) /
               static_cast<double>(latencies_ms.size());
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "  " << label << " latency:\n";
  std::cout << "    avg=" << avg << " ms, p50=" << Percentile(latencies_ms, 50)
            << " ms, p95=" << Percentile(latencies_ms, 95)
            << " ms, p99=" << Percentile(latencies_ms, 99)
            << " ms, max=" << latencies_ms.back() << " ms\n";
}

/**
 * @brief Build a 32-dimensional embedding vector for a given category
 *
 * Videos within the same category share a strong base signal with small
 * per-video perturbation, ensuring intra-category similarity.
 *
 * @param category Category index in [0, kNumCategories)
 * @param video_index Per-category video index (used for perturbation)
 * @return Space-separated string of 32 float components
 */
std::string BuildCategoryVector(size_t category, size_t video_index) {
  std::string result;
  result.reserve(kVectorDimension * 8);
  // Use a deterministic seed per (category, video_index) pair.
  std::mt19937 rng(static_cast<unsigned>(category * 100000 + video_index));
  std::uniform_real_distribution<float> noise(-0.05F, 0.05F);

  for (int d = 0; d < kVectorDimension; ++d) {
    if (d > 0) {
      result += ' ';
    }
    // Base signal: category determines which dimensions are "hot".
    float base = 0.0F;
    size_t dim_mod = static_cast<size_t>(d) % kNumCategories;
    if (dim_mod == category) {
      base = 0.8F;
    } else if (dim_mod == (category + 1) % kNumCategories) {
      base = 0.3F;
    }
    float value = base + noise(rng);
    result += std::to_string(value);
  }
  return result;
}

/**
 * @brief Generate a video ID string from category and index
 */
std::string VideoId(size_t category, size_t index) {
  return "vid_c" + std::to_string(category) + "_" + std::to_string(index);
}

}  // namespace

// ============================================================================
// Fixture
// ============================================================================

/**
 * @brief Test fixture for TikTok 1M-scale benchmark tests
 *
 * Configures the server for high concurrency and provides parallel bulk
 * loading helpers to efficiently populate datasets up to 1 million items.
 */
class TikTok1MScaleTest : public NvecdTestFixture {
 protected:
  void SetUp() override { SetUpServer(kVectorDimension); }
  void TearDown() override { TearDownServer(); }

  /**
   * @brief Override server config for 1M-scale tests
   *
   * Configures high connection limits, large thread pool, and expanded
   * context buffer to support parallel bulk loading and concurrent queries.
   */
  void SetUpServer(int dimension) {
    NvecdTestFixture::SetUpServer(dimension);
    if (server_) {
      server_->Stop();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    config_.perf.max_connections = 600;
    config_.perf.max_connections_per_ip = 0;  // Unlimited per-IP
    config_.perf.thread_pool_size = 16;
    config_.events.ctx_buffer_size = 1000;

    server_ = std::make_unique<nvecd::server::NvecdServer>(config_);
    ASSERT_TRUE(server_->Start().has_value());

    port_ = server_->GetPort();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  /**
   * @brief Load videos in parallel using multiple TCP clients
   *
   * Partitions the total count across num_loaders threads, each with its
   * own TCP connection. Videos are distributed evenly across kNumCategories.
   *
   * @param count Total number of videos to load
   * @param num_loaders Number of concurrent loader threads
   */
  void ParallelBulkLoadVideos(size_t count, size_t num_loaders = 16) {
    size_t per_loader = count / num_loaders;
    size_t remainder = count % num_loaders;
    std::atomic<size_t> total_loaded{0};

    std::cout << "  Loading " << count << " videos with " << num_loaders
              << " parallel loaders...\n";

    double load_ms = MeasureMs([&]() {
      std::vector<std::thread> threads;
      threads.reserve(num_loaders);

      size_t offset = 0;
      for (size_t loader = 0; loader < num_loaders; ++loader) {
        size_t chunk = per_loader + (loader < remainder ? 1 : 0);
        size_t start = offset;
        offset += chunk;

        threads.emplace_back([&, start, chunk]() {
          try {
            TcpClient client("127.0.0.1", port_);
            for (size_t i = 0; i < chunk; ++i) {
              size_t global_idx = start + i;
              size_t cat = global_idx % kNumCategories;
              size_t vid_idx = global_idx / kNumCategories;
              std::string id = VideoId(cat, vid_idx);
              std::string vec = BuildCategoryVector(cat, vid_idx);
              auto resp = client.SendCommand("VECSET " + id + " " + vec);
              if (ContainsOK(resp)) {
                total_loaded.fetch_add(1, std::memory_order_relaxed);
              }
            }
          } catch (const std::exception&) {
            // Connection failure -- partial load counted via atomic.
          }
        });
      }

      for (auto& t : threads) {
        t.join();
      }
    });

    size_t loaded = total_loaded.load();
    double throughput =
        (load_ms > 0)
            ? (static_cast<double>(loaded) / load_ms * 1000.0)
            : 0.0;
    std::cout << "  Loaded " << loaded << "/" << count << " videos in "
              << std::fixed << std::setprecision(1) << load_ms << " ms ("
              << throughput << " ops/sec)\n";
  }

  /**
   * @brief Seed co-occurrence events in parallel
   *
   * Creates contexts_count viewing contexts, each containing ~10 video
   * events from the same category. Distributes work across num_loaders.
   *
   * @param contexts_count Number of co-occurrence contexts to create
   * @param events_per_ctx Number of events per context
   * @param num_loaders Number of concurrent loader threads
   */
  void ParallelBulkSeedEvents(size_t contexts_count,
                              size_t events_per_ctx = 10,
                              size_t num_loaders = 16) {
    size_t per_loader = contexts_count / num_loaders;
    size_t remainder = contexts_count % num_loaders;
    std::atomic<size_t> total_events{0};

    std::cout << "  Seeding " << contexts_count << " contexts x "
              << events_per_ctx << " events with " << num_loaders
              << " loaders...\n";

    double seed_ms = MeasureMs([&]() {
      std::vector<std::thread> threads;
      threads.reserve(num_loaders);

      size_t offset = 0;
      for (size_t loader = 0; loader < num_loaders; ++loader) {
        size_t chunk = per_loader + (loader < remainder ? 1 : 0);
        size_t start = offset;
        offset += chunk;

        threads.emplace_back([&, start, chunk, events_per_ctx]() {
          try {
            TcpClient client("127.0.0.1", port_);
            for (size_t c = 0; c < chunk; ++c) {
              size_t ctx_idx = start + c;
              size_t cat = ctx_idx % kNumCategories;
              std::string ctx = "seed_ctx_" + std::to_string(ctx_idx);

              // Each context watches videos from the same category.
              std::mt19937 rng(static_cast<unsigned>(ctx_idx * 37 + 11));
              for (size_t e = 0; e < events_per_ctx; ++e) {
                size_t vid_idx = rng() % 1000;  // Pick from first 1000 per cat
                std::string id = VideoId(cat, vid_idx);
                int score = 100 - static_cast<int>(e) * 5;
                auto resp = client.SendCommand("EVENT " + ctx + " ADD " + id +
                                               " " + std::to_string(score));
                if (ContainsOK(resp)) {
                  total_events.fetch_add(1, std::memory_order_relaxed);
                }
              }
            }
          } catch (const std::exception&) {
            // Connection failure.
          }
        });
      }

      for (auto& t : threads) {
        t.join();
      }
    });

    size_t seeded = total_events.load();
    double throughput =
        (seed_ms > 0)
            ? (static_cast<double>(seeded) / seed_ms * 1000.0)
            : 0.0;
    std::cout << "  Seeded " << seeded << " events in " << std::fixed
              << std::setprecision(1) << seed_ms << " ms (" << throughput
              << " ops/sec)\n";
  }

  /**
   * @brief Get a list of popular video IDs (first few per category)
   */
  std::vector<std::string> GetPopularVideos(size_t per_category = 5) {
    std::vector<std::string> popular;
    popular.reserve(kNumCategories * per_category);
    for (size_t cat = 0; cat < kNumCategories; ++cat) {
      for (size_t idx = 0; idx < per_category; ++idx) {
        popular.push_back(VideoId(cat, idx));
      }
    }
    return popular;
  }
};

// ============================================================================
// Test 1: Scaling Profile - measure latency degradation from 10K to 1M
// ============================================================================

TEST_F(TikTok1MScaleTest, DISABLED_ScalingProfile) {
  PrintHeader("Scaling Profile: 10K -> 10M");

  const std::vector<size_t> kScalePoints = {10000, 50000, 100000, 500000,
                                            1000000, 5000000, 10000000};
  const size_t kQueryCount = 100;
  size_t loaded_so_far = 0;

  // Table header.
  std::cout << "\n  " << std::left << std::setw(10) << "Scale" << std::setw(14)
            << "Load ops/s" << std::setw(10) << "SIM-evt" << std::setw(10)
            << "SIM-vec" << std::setw(10) << "SIM-fus" << std::setw(10)
            << "SIMV" << std::setw(14) << "Memory" << "\n";
  std::cout << "  " << std::string(78, '-') << "\n";

  for (size_t scale : kScalePoints) {
    // Load the delta to reach this scale point.
    size_t delta = scale - loaded_so_far;
    if (delta > 0) {
      // Use a temporary fixture-level load. We partition the delta across
      // loaders, starting from the current offset.
      std::atomic<size_t> delta_loaded{0};
      double load_ms = MeasureMs([&]() {
        const size_t kLoaders = 16;
        size_t per_loader = delta / kLoaders;
        size_t rem = delta % kLoaders;
        std::vector<std::thread> threads;
        threads.reserve(kLoaders);

        size_t offset = loaded_so_far;
        for (size_t loader = 0; loader < kLoaders; ++loader) {
          size_t chunk = per_loader + (loader < rem ? 1 : 0);
          size_t start = offset;
          offset += chunk;

          threads.emplace_back([&, start, chunk]() {
            try {
              TcpClient client("127.0.0.1", port_);
              for (size_t i = 0; i < chunk; ++i) {
                size_t global_idx = start + i;
                size_t cat = global_idx % kNumCategories;
                size_t vid_idx = global_idx / kNumCategories;
                std::string id = VideoId(cat, vid_idx);
                std::string vec = BuildCategoryVector(cat, vid_idx);
                auto resp = client.SendCommand("VECSET " + id + " " + vec);
                if (ContainsOK(resp)) {
                  delta_loaded.fetch_add(1, std::memory_order_relaxed);
                }
              }
            } catch (const std::exception&) {
            }
          });
        }
        for (auto& t : threads) {
          t.join();
        }
      });

      loaded_so_far = scale;
      double load_throughput =
          (load_ms > 0)
              ? (static_cast<double>(delta_loaded.load()) / load_ms * 1000.0)
              : 0.0;

      // Measure SIM latencies for each mode.
      TcpClient query_client("127.0.0.1", port_);

      auto measure_sim_latency =
          [&](const std::string& mode) -> std::vector<double> {
        std::vector<double> latencies;
        latencies.reserve(kQueryCount);
        std::mt19937 rng(static_cast<unsigned>(scale + 42));
        for (size_t q = 0; q < kQueryCount; ++q) {
          size_t cat = rng() % kNumCategories;
          size_t vid_idx = rng() % (scale / kNumCategories);
          std::string probe = VideoId(cat, vid_idx);
          double lat = MeasureMs([&]() {
            query_client.SendCommand("SIM " + probe + " 10 using=" + mode);
          });
          latencies.push_back(lat);
        }
        std::sort(latencies.begin(), latencies.end());
        return latencies;
      };

      auto evt_lat = measure_sim_latency("events");
      auto vec_lat = measure_sim_latency("vectors");
      auto fus_lat = measure_sim_latency("fusion");

      // Measure SIMV latency.
      std::vector<double> simv_lat;
      simv_lat.reserve(kQueryCount);
      {
        std::mt19937 rng(static_cast<unsigned>(scale + 99));
        for (size_t q = 0; q < kQueryCount; ++q) {
          // Build a random query vector.
          size_t cat = rng() % kNumCategories;
          std::string vec = BuildCategoryVector(cat, rng() % 10000);
          double lat = MeasureMs([&]() {
            query_client.SendCommand("SIMV 10 " + vec);
          });
          simv_lat.push_back(lat);
        }
        std::sort(simv_lat.begin(), simv_lat.end());
      }

      // Get memory usage.
      auto info_resp = query_client.SendCommand("INFO");
      std::string memory_str = ParseResponseField(info_resp, "used_memory_bytes");
      if (memory_str.empty()) {
        memory_str = ParseResponseField(info_resp, "used_memory");
      }

      // Print row.
      std::cout << "  " << std::left << std::setw(10) << scale << std::fixed
                << std::setprecision(0) << std::setw(14) << load_throughput
                << std::setprecision(2) << std::setw(10)
                << Percentile(evt_lat, 50) << std::setw(10)
                << Percentile(vec_lat, 50) << std::setw(10)
                << Percentile(fus_lat, 50) << std::setw(10)
                << Percentile(simv_lat, 50) << std::setw(14) << memory_str
                << "\n";

      // Print detailed latency breakdown at each scale.
      std::cout << "    Events  - avg="
                << std::accumulate(evt_lat.begin(), evt_lat.end(), 0.0) /
                       static_cast<double>(evt_lat.size())
                << " p95=" << Percentile(evt_lat, 95)
                << " p99=" << Percentile(evt_lat, 99)
                << " max=" << evt_lat.back() << "\n";
      std::cout << "    Vectors - avg="
                << std::accumulate(vec_lat.begin(), vec_lat.end(), 0.0) /
                       static_cast<double>(vec_lat.size())
                << " p95=" << Percentile(vec_lat, 95)
                << " p99=" << Percentile(vec_lat, 99)
                << " max=" << vec_lat.back() << "\n";
      std::cout << "    Fusion  - avg="
                << std::accumulate(fus_lat.begin(), fus_lat.end(), 0.0) /
                       static_cast<double>(fus_lat.size())
                << " p95=" << Percentile(fus_lat, 95)
                << " p99=" << Percentile(fus_lat, 99)
                << " max=" << fus_lat.back() << "\n";
      std::cout << "    SIMV    - avg="
                << std::accumulate(simv_lat.begin(), simv_lat.end(), 0.0) /
                       static_cast<double>(simv_lat.size())
                << " p95=" << Percentile(simv_lat, 95)
                << " p99=" << Percentile(simv_lat, 99)
                << " max=" << simv_lat.back() << "\n";

      // At 1M and 10M, report scaling health.
      if (scale >= 1000000) {
        std::string scale_label = std::to_string(scale / 1000000) + "M";
        std::cout << "    [" << scale_label << "] Fusion p99="
                  << Percentile(fus_lat, 99) << " ms, Vector p99="
                  << Percentile(vec_lat, 99) << " ms, SIMV p99="
                  << Percentile(simv_lat, 99) << " ms\n";
      }
    }
  }

  std::cout << "\n  (latency values in ms, p50 shown in table)\n";
}

// ============================================================================
// Test 2: TikTok Live 1M with 500 viewers
// ============================================================================

TEST_F(TikTok1MScaleTest, DISABLED_TikTokLive1MWith500Viewers) {
  PrintHeader("TikTok Live: 1M Videos + 500 Concurrent Viewers");

  // Phase 1: Parallel bulk load.
  std::cout << "\n  Phase 1: Data loading\n";
  double load_ms = MeasureMs([&]() {
    ParallelBulkLoadVideos(1000000, 16);
    ParallelBulkSeedEvents(10000, 10, 16);
  });
  std::cout << "  Total data loading time: " << std::fixed
            << std::setprecision(1) << load_ms << " ms\n";

  auto popular_videos = GetPopularVideos();

  // Phase 2: 500 concurrent viewers.
  const size_t kViewers = 500;
  const size_t kIterationsPerViewer = 20;

  std::atomic<int> total_events{0};
  std::atomic<int> total_recommendations{0};
  std::atomic<int> total_sim_success{0};
  std::atomic<int> total_sim_attempts{0};

  std::vector<double> all_latencies;
  std::mutex latency_mu;

  std::cout << "\n  Phase 2: Launching " << kViewers
            << " viewer threads, " << kIterationsPerViewer
            << " iterations each...\n";

  double viewer_ms = MeasureMs([&]() {
    std::vector<std::thread> threads;
    threads.reserve(kViewers);

    for (size_t v = 0; v < kViewers; ++v) {
      threads.emplace_back([&, v]() {
        std::mt19937 rng(static_cast<unsigned>(v * 42 + 7));
        std::vector<double> local_latencies;
        local_latencies.reserve(kIterationsPerViewer);

        try {
          TcpClient client("127.0.0.1", port_);
          std::string viewer_ctx = "viewer_" + std::to_string(v);

          // Start by watching a random popular video.
          std::string current_video =
              popular_videos[rng() % popular_videos.size()];

          for (size_t iter = 0; iter < kIterationsPerViewer; ++iter) {
            // Record the view event.
            int score = 100 - static_cast<int>(iter);
            auto event_resp = client.SendCommand(
                "EVENT " + viewer_ctx + " ADD " + current_video + " " +
                std::to_string(score));
            if (ContainsOK(event_resp)) {
              total_events.fetch_add(1, std::memory_order_relaxed);
            }

            // Get fusion recommendations and measure latency.
            total_sim_attempts.fetch_add(1, std::memory_order_relaxed);
            std::string sim_resp;
            double lat = MeasureMs([&]() {
              sim_resp = client.SendCommand("SIM " + current_video +
                                            " 10 using=fusion");
            });
            local_latencies.push_back(lat);

            if (ContainsOK(sim_resp)) {
              total_sim_success.fetch_add(1, std::memory_order_relaxed);
              auto results = ParseSimResults(sim_resp);
              total_recommendations.fetch_add(static_cast<int>(results.size()),
                                              std::memory_order_relaxed);

              // Pick a random recommendation as the next video.
              if (!results.empty()) {
                current_video = results[rng() % results.size()].first;
              }
            }
          }
        } catch (const std::exception&) {
          // Connection failure -- counted via success counters.
        }

        std::lock_guard<std::mutex> lock(latency_mu);
        all_latencies.insert(all_latencies.end(), local_latencies.begin(),
                             local_latencies.end());
      });
    }

    for (auto& t : threads) {
      t.join();
    }
  });

  // Report.
  int events = total_events.load();
  int recs = total_recommendations.load();
  int sim_ok = total_sim_success.load();
  int sim_total = total_sim_attempts.load();
  double success_rate =
      (sim_total > 0)
          ? (static_cast<double>(sim_ok) / static_cast<double>(sim_total) *
             100.0)
          : 0.0;

  std::cout << "\n  --- Results ---\n";
  std::cout << "  Data loading time:        " << std::fixed
            << std::setprecision(1) << load_ms << " ms\n";
  std::cout << "  Viewer phase wall time:   " << viewer_ms << " ms\n";
  std::cout << "  Total events sent:        " << events << "\n";
  std::cout << "  Total recommendations:    " << recs << "\n";
  std::cout << "  SIM success rate:         " << std::setprecision(1)
            << success_rate << "% (" << sim_ok << "/" << sim_total << ")\n";
  PrintThroughput("Events", events, viewer_ms);
  PrintThroughput("SIM queries", sim_total, viewer_ms);
  PrintLatency("SIM fusion", all_latencies);

  EXPECT_GT(success_rate, 90.0) << "SIM success rate too low at 1M scale";
}

// ============================================================================
// Test 3: Burst traffic on 1M dataset
// ============================================================================

TEST_F(TikTok1MScaleTest, DISABLED_TikTokBurst1M) {
  PrintHeader("TikTok Burst Traffic on 1M Dataset");

  // Phase 0: Bulk load data.
  std::cout << "\n  Phase 0: Data loading\n";
  ParallelBulkLoadVideos(1000000, 16);
  ParallelBulkSeedEvents(5000, 10, 16);

  auto popular_videos = GetPopularVideos();

  // Phase 1: Normal load baseline.
  const size_t kNormalViewers = 100;
  const size_t kNormalIterations = 10;

  std::atomic<int> normal_success{0};
  std::atomic<int> normal_attempts{0};
  std::vector<double> normal_latencies;
  std::mutex normal_mu;

  std::cout << "\n  Phase 1: Normal load (" << kNormalViewers
            << " viewers, " << kNormalIterations << " iterations)...\n";

  double normal_ms = MeasureMs([&]() {
    std::vector<std::thread> threads;
    threads.reserve(kNormalViewers);

    for (size_t v = 0; v < kNormalViewers; ++v) {
      threads.emplace_back([&, v]() {
        std::mt19937 rng(static_cast<unsigned>(v));
        std::vector<double> local_lat;
        local_lat.reserve(kNormalIterations);

        try {
          TcpClient client("127.0.0.1", port_);
          std::string ctx = "norm_" + std::to_string(v);

          for (size_t iter = 0; iter < kNormalIterations; ++iter) {
            std::string vid = popular_videos[rng() % popular_videos.size()];
            client.SendCommand("EVENT " + ctx + " ADD " + vid + " 80");

            normal_attempts.fetch_add(1, std::memory_order_relaxed);
            std::string resp;
            double lat = MeasureMs([&]() {
              resp =
                  client.SendCommand("SIM " + vid + " 10 using=fusion");
            });
            local_lat.push_back(lat);

            if (ContainsOK(resp)) {
              normal_success.fetch_add(1, std::memory_order_relaxed);
            }
          }
        } catch (const std::exception&) {
        }

        std::lock_guard<std::mutex> lock(normal_mu);
        normal_latencies.insert(normal_latencies.end(), local_lat.begin(),
                                local_lat.end());
      });
    }

    for (auto& t : threads) {
      t.join();
    }
  });

  int n_ok = normal_success.load();
  int n_total = normal_attempts.load();
  double normal_rate =
      (n_total > 0)
          ? (static_cast<double>(n_ok) / static_cast<double>(n_total) * 100.0)
          : 0.0;

  std::cout << "  Normal phase: " << std::fixed << std::setprecision(1)
            << normal_ms << " ms, success " << normal_rate << "%\n";
  PrintLatency("Normal SIM", normal_latencies);

  // Phase 2: Burst - 500 viewers all hit the same viral video.
  const size_t kBurstViewers = 500;
  std::string viral_video = VideoId(0, 0);

  std::atomic<int> burst_event_success{0};
  std::atomic<int> burst_sim_success{0};
  std::atomic<int> burst_sim_attempts{0};
  std::vector<double> burst_latencies;
  std::mutex burst_mu;

  std::cout << "\n  Phase 2: BURST! " << kBurstViewers
            << " viewers on viral video '" << viral_video
            << "' (1M dataset)...\n";

  double burst_ms = MeasureMs([&]() {
    std::vector<std::thread> threads;
    threads.reserve(kBurstViewers);

    for (size_t v = 0; v < kBurstViewers; ++v) {
      threads.emplace_back([&, v]() {
        try {
          TcpClient client("127.0.0.1", port_);
          std::string ctx = "burst_" + std::to_string(v);

          auto event_resp = client.SendCommand("EVENT " + ctx + " ADD " +
                                               viral_video + " 100");
          if (ContainsOK(event_resp)) {
            burst_event_success.fetch_add(1, std::memory_order_relaxed);
          }

          burst_sim_attempts.fetch_add(1, std::memory_order_relaxed);
          std::string sim_resp;
          double lat = MeasureMs([&]() {
            sim_resp = client.SendCommand("SIM " + viral_video +
                                          " 10 using=fusion");
          });

          {
            std::lock_guard<std::mutex> lock(burst_mu);
            burst_latencies.push_back(lat);
          }

          if (ContainsOK(sim_resp)) {
            burst_sim_success.fetch_add(1, std::memory_order_relaxed);
          }
        } catch (const std::exception&) {
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }
  });

  int b_events = burst_event_success.load();
  int b_sim_ok = burst_sim_success.load();
  int b_sim_total = burst_sim_attempts.load();
  double burst_rate =
      (b_sim_total > 0)
          ? (static_cast<double>(b_sim_ok) / static_cast<double>(b_sim_total) *
             100.0)
          : 0.0;

  std::cout << "\n  --- Burst Results ---\n";
  std::cout << "  Burst duration:           " << std::fixed
            << std::setprecision(1) << burst_ms << " ms\n";
  std::cout << "  Events recorded:          " << b_events << "/"
            << kBurstViewers << "\n";
  std::cout << "  SIM success rate:         " << burst_rate << "% ("
            << b_sim_ok << "/" << b_sim_total << ")\n";
  PrintLatency("Burst SIM", burst_latencies);

  // Compare normal vs burst latency.
  if (!normal_latencies.empty() && !burst_latencies.empty()) {
    std::sort(normal_latencies.begin(), normal_latencies.end());
    std::sort(burst_latencies.begin(), burst_latencies.end());
    double normal_p50 = Percentile(normal_latencies, 50);
    double normal_p99 = Percentile(normal_latencies, 99);
    double burst_p50 = Percentile(burst_latencies, 50);
    double burst_p99 = Percentile(burst_latencies, 99);
    double degradation_p50 =
        (normal_p50 > 0) ? (burst_p50 / normal_p50) : 0.0;
    double degradation_p99 =
        (normal_p99 > 0) ? (burst_p99 / normal_p99) : 0.0;

    std::cout << "\n  --- Latency Comparison (1M dataset) ---\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  Normal p50=" << normal_p50 << " ms, p99=" << normal_p99
              << " ms\n";
    std::cout << "  Burst  p50=" << burst_p50 << " ms, p99=" << burst_p99
              << " ms\n";
    std::cout << std::setprecision(1);
    std::cout << "  Degradation: p50=" << degradation_p50
              << "x, p99=" << degradation_p99 << "x\n";
  }

  EXPECT_GT(burst_rate, 80.0) << "Burst SIM success rate too low at 1M scale";
}

// ============================================================================
// Test 4: Memory Profile at scale
// ============================================================================

TEST_F(TikTok1MScaleTest, DISABLED_MemoryProfile1M) {
  PrintHeader("Memory Profile: 10K -> 1M");

  const std::vector<size_t> kScalePoints = {10000, 50000, 100000, 500000,
                                            1000000};
  size_t loaded_so_far = 0;

  // Table header.
  std::cout << "\n  " << std::left << std::setw(10) << "Scale"
            << std::setw(18) << "Memory (bytes)" << std::setw(14)
            << "Vectors" << std::setw(14) << "Events" << std::setw(14)
            << "Contexts" << std::setw(14) << "Bytes/Video" << "\n";
  std::cout << "  " << std::string(84, '-') << "\n";

  for (size_t scale : kScalePoints) {
    size_t delta = scale - loaded_so_far;
    if (delta > 0) {
      // Load delta videos.
      std::atomic<size_t> delta_loaded{0};
      const size_t kLoaders = 16;
      size_t per_loader = delta / kLoaders;
      size_t rem = delta % kLoaders;

      std::vector<std::thread> threads;
      threads.reserve(kLoaders);

      size_t offset = loaded_so_far;
      for (size_t loader = 0; loader < kLoaders; ++loader) {
        size_t chunk = per_loader + (loader < rem ? 1 : 0);
        size_t start = offset;
        offset += chunk;

        threads.emplace_back([&, start, chunk]() {
          try {
            TcpClient client("127.0.0.1", port_);
            for (size_t i = 0; i < chunk; ++i) {
              size_t global_idx = start + i;
              size_t cat = global_idx % kNumCategories;
              size_t vid_idx = global_idx / kNumCategories;
              std::string id = VideoId(cat, vid_idx);
              std::string vec = BuildCategoryVector(cat, vid_idx);
              auto resp = client.SendCommand("VECSET " + id + " " + vec);
              if (ContainsOK(resp)) {
                delta_loaded.fetch_add(1, std::memory_order_relaxed);
              }
            }
          } catch (const std::exception&) {
          }
        });
      }
      for (auto& t : threads) {
        t.join();
      }

      // Also seed some events proportional to scale.
      size_t event_contexts = scale / 100;  // 1% of videos get contexts
      if (event_contexts > 0 && loaded_so_far == 0) {
        // Only seed events on the first pass; subsequent loads just add vectors.
        ParallelBulkSeedEvents(event_contexts, 10, 16);
      }

      loaded_so_far = scale;
    }

    // Query INFO for memory and counts.
    TcpClient info_client("127.0.0.1", port_);
    auto info_resp = info_client.SendCommand("INFO");

    std::string memory_str =
        ParseResponseField(info_resp, "used_memory_bytes");
    if (memory_str.empty()) {
      memory_str = ParseResponseField(info_resp, "used_memory");
    }
    std::string vector_count_str =
        ParseResponseField(info_resp, "vector_count");
    std::string event_count_str =
        ParseResponseField(info_resp, "event_count");
    std::string context_count_str =
        ParseResponseField(info_resp, "context_count");

    // Calculate bytes per video.
    long long memory_bytes = 0;
    if (!memory_str.empty()) {
      try {
        memory_bytes = std::stoll(memory_str);
      } catch (const std::exception&) {
        // Could not parse memory.
      }
    }
    double bytes_per_video =
        (scale > 0 && memory_bytes > 0)
            ? (static_cast<double>(memory_bytes) / static_cast<double>(scale))
            : 0.0;

    // Print row.
    std::cout << "  " << std::left << std::setw(10) << scale << std::setw(18)
              << memory_str << std::setw(14) << vector_count_str
              << std::setw(14) << event_count_str << std::setw(14)
              << context_count_str << std::fixed << std::setprecision(1)
              << std::setw(14) << bytes_per_video << "\n";
  }

  std::cout << "\n  Memory profile complete.\n";
}

// ============================================================================
// Test 5: TikTok Live 10M with 500 viewers
// ============================================================================

TEST_F(TikTok1MScaleTest, DISABLED_TikTokLive10MWith500Viewers) {
  PrintHeader("TikTok Live: 10M Videos + 500 Concurrent Viewers");

  // Phase 1: Parallel bulk load 10M videos.
  std::cout << "\n  Phase 1: Data loading (10M videos)\n";
  double load_ms = MeasureMs([&]() {
    ParallelBulkLoadVideos(10000000, 16);
    ParallelBulkSeedEvents(10000, 10, 16);
  });
  std::cout << "  Total data loading time: " << std::fixed
            << std::setprecision(1) << load_ms << " ms ("
            << std::setprecision(1) << load_ms / 1000.0 << " sec)\n";

  // Check memory usage after loading.
  {
    TcpClient info_client("127.0.0.1", port_);
    auto info = info_client.SendCommand("INFO");
    std::string mem = ParseResponseField(info, "used_memory_bytes");
    std::string vecs = ParseResponseField(info, "vector_count");
    std::cout << "  Loaded vectors: " << vecs << ", Memory: " << mem
              << " bytes ("
              << (mem.empty() ? 0.0
                              : std::stod(mem) / 1024.0 / 1024.0)
              << " MB)\n";
  }

  auto popular_videos = GetPopularVideos();

  // Phase 2: 500 concurrent viewers.
  const size_t kViewers = 500;
  const size_t kIterationsPerViewer = 20;

  std::atomic<int> total_events{0};
  std::atomic<int> total_recommendations{0};
  std::atomic<int> total_sim_success{0};
  std::atomic<int> total_sim_attempts{0};

  std::vector<double> all_latencies;
  std::mutex latency_mu;

  std::cout << "\n  Phase 2: Launching " << kViewers
            << " viewer threads, " << kIterationsPerViewer
            << " iterations each...\n";

  double viewer_ms = MeasureMs([&]() {
    std::vector<std::thread> threads;
    threads.reserve(kViewers);

    for (size_t v = 0; v < kViewers; ++v) {
      threads.emplace_back([&, v]() {
        std::mt19937 rng(static_cast<unsigned>(v * 42 + 7));
        std::vector<double> local_latencies;
        local_latencies.reserve(kIterationsPerViewer);

        try {
          TcpClient client("127.0.0.1", port_);
          std::string viewer_ctx = "viewer_" + std::to_string(v);
          std::string current_video =
              popular_videos[rng() % popular_videos.size()];

          for (size_t iter = 0; iter < kIterationsPerViewer; ++iter) {
            int score = 100 - static_cast<int>(iter);
            auto event_resp = client.SendCommand(
                "EVENT " + viewer_ctx + " ADD " + current_video + " " +
                std::to_string(score));
            if (ContainsOK(event_resp)) {
              total_events.fetch_add(1, std::memory_order_relaxed);
            }

            total_sim_attempts.fetch_add(1, std::memory_order_relaxed);
            std::string sim_resp;
            double lat = MeasureMs([&]() {
              sim_resp = client.SendCommand("SIM " + current_video +
                                            " 10 using=fusion");
            });
            local_latencies.push_back(lat);

            if (ContainsOK(sim_resp)) {
              total_sim_success.fetch_add(1, std::memory_order_relaxed);
              auto results = ParseSimResults(sim_resp);
              total_recommendations.fetch_add(
                  static_cast<int>(results.size()),
                  std::memory_order_relaxed);
              if (!results.empty()) {
                current_video = results[rng() % results.size()].first;
              }
            }
          }
        } catch (const std::exception&) {
        }

        std::lock_guard<std::mutex> lock(latency_mu);
        all_latencies.insert(all_latencies.end(), local_latencies.begin(),
                             local_latencies.end());
      });
    }

    for (auto& t : threads) {
      t.join();
    }
  });

  int events = total_events.load();
  int recs = total_recommendations.load();
  int sim_ok = total_sim_success.load();
  int sim_total = total_sim_attempts.load();
  double success_rate =
      (sim_total > 0)
          ? (static_cast<double>(sim_ok) / static_cast<double>(sim_total) *
             100.0)
          : 0.0;

  std::cout << "\n  --- Results (10M dataset) ---\n";
  std::cout << "  Data loading time:        " << std::fixed
            << std::setprecision(1) << load_ms / 1000.0 << " sec\n";
  std::cout << "  Viewer phase wall time:   " << viewer_ms << " ms\n";
  std::cout << "  Total events sent:        " << events << "\n";
  std::cout << "  Total recommendations:    " << recs << "\n";
  std::cout << "  SIM success rate:         " << std::setprecision(1)
            << success_rate << "% (" << sim_ok << "/" << sim_total << ")\n";
  PrintThroughput("Events", events, viewer_ms);
  PrintThroughput("SIM queries", sim_total, viewer_ms);
  PrintLatency("SIM fusion", all_latencies);

  // 10M with brute-force: SIM fusion latency will be ~30ms+
  // Report whether this is still viable or needs ANN index.
  if (!all_latencies.empty()) {
    std::sort(all_latencies.begin(), all_latencies.end());
    double p50 = Percentile(all_latencies, 50);
    double p99 = Percentile(all_latencies, 99);
    std::cout << "\n  --- 10M Viability Assessment ---\n";
    if (p99 < 100.0) {
      std::cout << "  PASS: p99=" << p99
                << " ms - viable for real-time recommendations\n";
    } else if (p99 < 500.0) {
      std::cout << "  WARNING: p99=" << p99
                << " ms - marginal, consider ANN index (FAISS)\n";
    } else {
      std::cout << "  CRITICAL: p99=" << p99
                << " ms - ANN index (FAISS) required for production\n";
    }
    std::cout << "  Estimated throughput at p50: "
              << std::setprecision(0) << (1000.0 / p50)
              << " queries/sec per thread\n";
  }

  EXPECT_GT(success_rate, 90.0) << "SIM success rate too low at 10M scale";
}

// ============================================================================
// Test 6: Memory Profile up to 10M
// ============================================================================

TEST_F(TikTok1MScaleTest, DISABLED_MemoryProfile10M) {
  PrintHeader("Memory Profile: 10K -> 10M");

  const std::vector<size_t> kScalePoints = {10000, 50000, 100000, 500000,
                                            1000000, 5000000, 10000000};
  size_t loaded_so_far = 0;

  std::cout << "\n  " << std::left << std::setw(12) << "Scale"
            << std::setw(18) << "Memory (bytes)" << std::setw(10)
            << "MB" << std::setw(14) << "Vectors" << std::setw(14)
            << "Bytes/Video" << "\n";
  std::cout << "  " << std::string(68, '-') << "\n";

  for (size_t scale : kScalePoints) {
    size_t delta = scale - loaded_so_far;
    if (delta > 0) {
      std::atomic<size_t> delta_loaded{0};
      const size_t kLoaders = 16;
      size_t per_loader = delta / kLoaders;
      size_t rem = delta % kLoaders;

      std::vector<std::thread> threads;
      threads.reserve(kLoaders);

      size_t offset = loaded_so_far;
      for (size_t loader = 0; loader < kLoaders; ++loader) {
        size_t chunk = per_loader + (loader < rem ? 1 : 0);
        size_t start = offset;
        offset += chunk;

        threads.emplace_back([&, start, chunk]() {
          try {
            TcpClient client("127.0.0.1", port_);
            for (size_t i = 0; i < chunk; ++i) {
              size_t global_idx = start + i;
              size_t cat = global_idx % kNumCategories;
              size_t vid_idx = global_idx / kNumCategories;
              std::string id = VideoId(cat, vid_idx);
              std::string vec = BuildCategoryVector(cat, vid_idx);
              client.SendCommand("VECSET " + id + " " + vec);
            }
          } catch (const std::exception&) {
          }
        });
      }
      for (auto& t : threads) {
        t.join();
      }
      loaded_so_far = scale;
    }

    TcpClient info_client("127.0.0.1", port_);
    auto info_resp = info_client.SendCommand("INFO");
    std::string memory_str =
        ParseResponseField(info_resp, "used_memory_bytes");
    std::string vector_count_str =
        ParseResponseField(info_resp, "vector_count");

    long long memory_bytes = 0;
    if (!memory_str.empty()) {
      try {
        memory_bytes = std::stoll(memory_str);
      } catch (const std::exception&) {
      }
    }
    double mb = static_cast<double>(memory_bytes) / 1024.0 / 1024.0;
    double bytes_per_video =
        (scale > 0 && memory_bytes > 0)
            ? (static_cast<double>(memory_bytes) / static_cast<double>(scale))
            : 0.0;

    std::cout << "  " << std::left << std::setw(12) << scale
              << std::setw(18) << memory_str << std::fixed
              << std::setprecision(1) << std::setw(10) << mb
              << std::setw(14) << vector_count_str << std::setw(14)
              << bytes_per_video << "\n";
  }

  std::cout << "\n  Memory profile complete.\n";
}

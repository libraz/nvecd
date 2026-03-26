/**
 * @file tiktok_scenario_e2e_test.cpp
 * @brief TikTok-like scenario E2E tests simulating realistic recommendation workloads
 *
 * Simulates a TikTok-style video platform with concurrent viewers requesting
 * fusion-based recommendations while new content is continuously uploaded.
 * Tests validate system behavior under high concurrency (500 viewers),
 * burst traffic, and recommendation quality through co-occurrence patterns.
 *
 * All tests are DISABLED by default (CI-safe). Run with:
 *   --gtest_also_run_disabled_tests --gtest_filter="TikTokScenarioE2ETest.*"
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
// Helper functions (copied from performance_e2e_test.cpp)
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
  size_t idx =
      static_cast<size_t>(p / 100.0 * static_cast<double>(sorted_values.size() - 1));
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
      (duration_ms > 0) ? (static_cast<double>(ops) / duration_ms * 1000.0) : 0.0;
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "  " << label << ": " << ops << " ops in " << duration_ms << " ms ("
            << ops_per_sec << " ops/sec)\n";
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
    // Each category has ~3 strong dimensions spread across the 32-dim space.
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
 * @brief Test fixture for TikTok-like scenario E2E tests
 *
 * Configures the server to support 500+ concurrent connections with
 * 32-dimensional vectors and a large context buffer for high-volume
 * co-occurrence tracking.
 */
class TikTokScenarioE2ETest : public NvecdTestFixture {
 protected:
  void SetUp() override { SetUpServer(kVectorDimension); }
  void TearDown() override { TearDownServer(); }

  /**
   * @brief Override server config to support 500+ concurrent connections
   */
  void SetUpServer(int dimension) {
    NvecdTestFixture::SetUpServer(dimension);
    // The base fixture already started the server with default limits.
    // We need to stop and restart with higher limits.
    if (server_) {
      server_->Stop();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    config_.perf.max_connections = 600;
    config_.perf.max_connections_per_ip = 0;  // Unlimited per-IP (all connections from 127.0.0.1)
    config_.perf.thread_pool_size = 16;
    config_.events.ctx_buffer_size = 1000;

    server_ = std::make_unique<nvecd::server::NvecdServer>(config_);
    ASSERT_TRUE(server_->Start().has_value());

    port_ = server_->GetPort();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  /**
   * @brief Pre-populate the catalog with ~2000 videos across 10 categories
   *
   * Each category gets 200 videos with similar 32-dim embeddings.
   * Also seeds initial co-occurrence data for popular video pairs.
   *
   * @param client TCP client for issuing commands
   * @param videos_per_category Number of videos per category (default 200)
   */
  void PopulateCatalog(TcpClient& client, size_t videos_per_category = 200) {
    std::cout << "  Populating catalog: " << kNumCategories << " categories x "
              << videos_per_category << " videos = "
              << kNumCategories * videos_per_category << " total\n";

    // Upload all video embeddings.
    for (size_t cat = 0; cat < kNumCategories; ++cat) {
      for (size_t idx = 0; idx < videos_per_category; ++idx) {
        std::string id = VideoId(cat, idx);
        std::string vec = BuildCategoryVector(cat, idx);
        client.SendCommand("VECSET " + id + " " + vec);
      }
    }

    // Seed co-occurrence data: within each category, simulate 20 viewing
    // contexts where the top-10 popular videos co-occur.
    std::cout << "  Seeding co-occurrence events...\n";
    for (size_t cat = 0; cat < kNumCategories; ++cat) {
      for (size_t ctx_idx = 0; ctx_idx < 20; ++ctx_idx) {
        std::string ctx =
            "seed_c" + std::to_string(cat) + "_" + std::to_string(ctx_idx);
        // Each context watches ~10 popular videos from this category.
        size_t start_video = (ctx_idx * 5) % videos_per_category;
        for (size_t v = 0; v < 10; ++v) {
          size_t vid_idx = (start_video + v) % videos_per_category;
          std::string id = VideoId(cat, vid_idx);
          int score = 100 - static_cast<int>(v) * 5;
          client.SendCommand("EVENT " + ctx + " ADD " + id + " " +
                             std::to_string(score));
        }
      }
    }

    std::cout << "  Catalog populated.\n";
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
// Test 1: TikTok Live - 500 concurrent viewers
// ============================================================================

TEST_F(TikTokScenarioE2ETest, DISABLED_TikTokLive500Viewers) {
  PrintHeader("TikTok Live: 500 Concurrent Viewers");

  TcpClient setup("127.0.0.1", port_);
  PopulateCatalog(setup);

  auto popular_videos = GetPopularVideos();

  const size_t kViewers = 500;
  const size_t kIterationsPerViewer = 20;

  std::atomic<int> total_events{0};
  std::atomic<int> total_recommendations{0};
  std::atomic<int> total_sim_success{0};
  std::atomic<int> total_sim_attempts{0};

  // Collect latencies with a mutex-guarded vector.
  std::vector<double> all_latencies;
  std::mutex latency_mu;

  std::cout << "  Launching " << kViewers << " viewer threads...\n";

  double total_ms = MeasureMs([&]() {
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

              // Pick a random recommendation as the next video to watch.
              if (!results.empty()) {
                current_video =
                    results[rng() % results.size()].first;
              }
            }
          }
        } catch (const std::exception& e) {
          // Connection failure -- counted via success counters.
        }

        // Merge local latencies.
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
          ? (static_cast<double>(sim_ok) / static_cast<double>(sim_total) * 100.0)
          : 0.0;

  std::cout << "\n  --- Results ---\n";
  std::cout << "  Total wall time:          " << std::fixed << std::setprecision(1)
            << total_ms << " ms\n";
  std::cout << "  Total events sent:        " << events << "\n";
  std::cout << "  Total recommendations:    " << recs << "\n";
  std::cout << "  SIM success rate:         " << std::setprecision(1) << success_rate
            << "% (" << sim_ok << "/" << sim_total << ")\n";
  PrintThroughput("Events", events, total_ms);
  PrintThroughput("SIM queries", sim_total, total_ms);
  PrintLatency("SIM fusion", all_latencies);

  // Assertions: expect high success rate.
  EXPECT_GT(success_rate, 90.0) << "SIM success rate too low";
  EXPECT_GT(events, 0) << "No events were recorded";
}

// ============================================================================
// Test 2: TikTok Live with creators uploading new content
// ============================================================================

TEST_F(TikTokScenarioE2ETest, DISABLED_TikTokLiveWithCreators) {
  PrintHeader("TikTok Live: 490 Viewers + 10 Creators");

  TcpClient setup("127.0.0.1", port_);
  PopulateCatalog(setup);

  auto popular_videos = GetPopularVideos();

  const size_t kViewers = 490;
  const size_t kCreators = 10;
  const size_t kViewerIterations = 20;
  const size_t kVideosPerCreator = 50;

  std::atomic<int> total_events{0};
  std::atomic<int> total_sim_success{0};
  std::atomic<int> total_sim_attempts{0};
  std::atomic<int> total_uploads{0};
  std::atomic<int> total_upload_success{0};

  std::vector<double> viewer_latencies;
  std::mutex latency_mu;

  std::cout << "  Launching " << kViewers << " viewers and " << kCreators
            << " creators...\n";

  double total_ms = MeasureMs([&]() {
    std::vector<std::thread> threads;
    threads.reserve(kViewers + kCreators);

    // Viewer threads.
    for (size_t v = 0; v < kViewers; ++v) {
      threads.emplace_back([&, v]() {
        std::mt19937 rng(static_cast<unsigned>(v * 31 + 13));
        std::vector<double> local_latencies;
        local_latencies.reserve(kViewerIterations);

        try {
          TcpClient client("127.0.0.1", port_);
          std::string viewer_ctx = "vw_" + std::to_string(v);
          std::string current_video =
              popular_videos[rng() % popular_videos.size()];

          for (size_t iter = 0; iter < kViewerIterations; ++iter) {
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
              if (!results.empty()) {
                current_video = results[rng() % results.size()].first;
              }
            }
          }
        } catch (const std::exception&) {
          // Connection failure.
        }

        std::lock_guard<std::mutex> lock(latency_mu);
        viewer_latencies.insert(viewer_latencies.end(), local_latencies.begin(),
                                local_latencies.end());
      });
    }

    // Creator threads.
    for (size_t c = 0; c < kCreators; ++c) {
      threads.emplace_back([&, c]() {
        try {
          TcpClient client("127.0.0.1", port_);

          for (size_t vid = 0; vid < kVideosPerCreator; ++vid) {
            std::string id =
                "new_c" + std::to_string(c) + "_" + std::to_string(vid);
            // Assign new videos to a random category.
            size_t cat = (c + vid) % kNumCategories;
            std::string vec = BuildCategoryVector(cat, 10000 + c * 1000 + vid);

            total_uploads.fetch_add(1, std::memory_order_relaxed);
            auto resp = client.SendCommand("VECSET " + id + " " + vec);
            if (ContainsOK(resp)) {
              total_upload_success.fetch_add(1, std::memory_order_relaxed);
            }

            // Small delay to spread uploads over the test duration.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
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

  // Report.
  int events = total_events.load();
  int sim_ok = total_sim_success.load();
  int sim_total = total_sim_attempts.load();
  int uploads = total_uploads.load();
  int uploads_ok = total_upload_success.load();
  double sim_rate =
      (sim_total > 0)
          ? (static_cast<double>(sim_ok) / static_cast<double>(sim_total) * 100.0)
          : 0.0;
  double upload_rate =
      (uploads > 0)
          ? (static_cast<double>(uploads_ok) / static_cast<double>(uploads) * 100.0)
          : 0.0;

  std::cout << "\n  --- Results ---\n";
  std::cout << "  Total wall time:          " << std::fixed << std::setprecision(1)
            << total_ms << " ms\n";
  std::cout << "  Events sent:              " << events << "\n";
  std::cout << "  SIM success rate:         " << std::setprecision(1) << sim_rate
            << "% (" << sim_ok << "/" << sim_total << ")\n";
  std::cout << "  Videos uploaded:           " << uploads_ok << "/" << uploads
            << " (" << std::setprecision(1) << upload_rate << "%)\n";
  PrintThroughput("Events", events, total_ms);
  PrintThroughput("SIM queries", sim_total, total_ms);
  PrintLatency("SIM fusion (viewers)", viewer_latencies);

  // Verify new content is searchable.
  TcpClient verify("127.0.0.1", port_);
  auto resp = verify.SendCommand("SIM new_c0_0 5 using=vectors");
  std::cout << "  Post-test SIM on new content: "
            << (ContainsOK(resp) ? "OK" : "FAIL") << "\n";

  EXPECT_GT(sim_rate, 85.0) << "SIM success rate too low under mixed load";
  EXPECT_GT(upload_rate, 95.0) << "Upload success rate too low";
  EXPECT_TRUE(ContainsOK(resp)) << "New content not searchable after test";
}

// ============================================================================
// Test 3: Recommendation quality through co-occurrence
// ============================================================================

TEST_F(TikTokScenarioE2ETest, DISABLED_TikTokRecommendationQuality) {
  PrintHeader("TikTok Recommendation Quality");

  TcpClient setup("127.0.0.1", port_);
  const size_t kVideosPerCategory = 200;
  PopulateCatalog(setup, kVideosPerCategory);

  const size_t kViewers = 200;
  const size_t kWatchesPerViewer = 50;

  std::atomic<int> total_events{0};

  std::cout << "  Phase 1: " << kViewers
            << " viewers each watching 50 same-category videos...\n";

  // Each viewer is assigned a category and watches only videos from it.
  double watch_ms = MeasureMs([&]() {
    std::vector<std::thread> threads;
    threads.reserve(kViewers);

    for (size_t v = 0; v < kViewers; ++v) {
      threads.emplace_back([&, v]() {
        try {
          TcpClient client("127.0.0.1", port_);
          size_t cat = v % kNumCategories;
          std::string ctx = "qual_v" + std::to_string(v);
          std::mt19937 rng(static_cast<unsigned>(v * 17 + 3));

          for (size_t w = 0; w < kWatchesPerViewer; ++w) {
            size_t vid_idx = rng() % kVideosPerCategory;
            std::string id = VideoId(cat, vid_idx);
            int score = 100 - static_cast<int>(w);
            auto resp = client.SendCommand("EVENT " + ctx + " ADD " + id +
                                           " " + std::to_string(score));
            if (ContainsOK(resp)) {
              total_events.fetch_add(1, std::memory_order_relaxed);
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

  std::cout << "  Watch phase: " << total_events.load() << " events in "
            << std::fixed << std::setprecision(1) << watch_ms << " ms\n";

  // Phase 2: Check recommendation quality.
  std::cout << "  Phase 2: Checking recommendation relevance...\n";

  TcpClient query("127.0.0.1", port_);
  size_t total_results = 0;
  size_t same_category_results = 0;

  for (size_t cat = 0; cat < kNumCategories; ++cat) {
    // Query SIM for a representative video in each category.
    std::string probe_id = VideoId(cat, 0);
    auto resp = query.SendCommand("SIM " + probe_id + " 10 using=fusion");
    if (!ContainsOK(resp)) {
      continue;
    }

    auto results = ParseSimResults(resp);
    total_results += results.size();

    // Check how many results belong to the same category.
    std::string cat_prefix = "vid_c" + std::to_string(cat) + "_";
    for (const auto& [id, score] : results) {
      if (id.find(cat_prefix) == 0) {
        ++same_category_results;
      }
    }

    std::cout << "    Category " << cat << " (" << probe_id << "): "
              << results.size() << " results";
    size_t cat_count = 0;
    for (const auto& [id, score] : results) {
      if (id.find(cat_prefix) == 0) {
        ++cat_count;
      }
    }
    std::cout << ", " << cat_count << " same-category\n";
  }

  double relevance_pct =
      (total_results > 0)
          ? (static_cast<double>(same_category_results) /
             static_cast<double>(total_results) * 100.0)
          : 0.0;

  std::cout << "\n  --- Quality Summary ---\n";
  std::cout << "  Same-category results: " << same_category_results << "/"
            << total_results << " (" << std::fixed << std::setprecision(1)
            << relevance_pct << "%)\n";

  // With strong co-occurrence signals, expect at least 40% same-category
  // results. This is conservative; fusion search combines both vector
  // similarity and co-occurrence, so it should beat random (10%).
  EXPECT_GT(relevance_pct, 40.0)
      << "Recommendation relevance too low; co-occurrence not contributing";
}

// ============================================================================
// Test 4: Burst traffic (viral moment)
// ============================================================================

TEST_F(TikTokScenarioE2ETest, DISABLED_TikTokBurstTraffic) {
  PrintHeader("TikTok Burst Traffic (Viral Moment)");

  TcpClient setup("127.0.0.1", port_);
  PopulateCatalog(setup);

  // Pick a "viral" video.
  std::string viral_video = VideoId(0, 0);

  // Phase 1: Normal load - 100 viewers for ~2 seconds.
  const size_t kNormalViewers = 100;
  const size_t kNormalIterations = 10;

  std::atomic<int> normal_success{0};
  std::atomic<int> normal_attempts{0};
  std::vector<double> normal_latencies;
  std::mutex normal_mu;

  std::cout << "  Phase 1: Normal load (" << kNormalViewers
            << " viewers)...\n";

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
          auto popular = GetPopularVideos();

          for (size_t iter = 0; iter < kNormalIterations; ++iter) {
            std::string vid = popular[rng() % popular.size()];
            client.SendCommand("EVENT " + ctx + " ADD " + vid + " 80");

            normal_attempts.fetch_add(1, std::memory_order_relaxed);
            std::string resp;
            double lat = MeasureMs([&]() {
              resp = client.SendCommand("SIM " + vid + " 10 using=fusion");
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

  // Phase 2: Burst - 500 viewers all hit the viral video simultaneously.
  const size_t kBurstViewers = 500;

  std::atomic<int> burst_event_success{0};
  std::atomic<int> burst_sim_success{0};
  std::atomic<int> burst_sim_attempts{0};
  std::vector<double> burst_latencies;
  std::mutex burst_mu;

  std::cout << "\n  Phase 2: BURST! " << kBurstViewers
            << " viewers on viral video '" << viral_video << "'...\n";

  double burst_ms = MeasureMs([&]() {
    std::vector<std::thread> threads;
    threads.reserve(kBurstViewers);

    for (size_t v = 0; v < kBurstViewers; ++v) {
      threads.emplace_back([&, v]() {
        try {
          TcpClient client("127.0.0.1", port_);
          std::string ctx = "burst_" + std::to_string(v);

          // Everyone watches the viral video.
          auto event_resp = client.SendCommand("EVENT " + ctx + " ADD " +
                                               viral_video + " 100");
          if (ContainsOK(event_resp)) {
            burst_event_success.fetch_add(1, std::memory_order_relaxed);
          }

          // Everyone requests recommendations.
          burst_sim_attempts.fetch_add(1, std::memory_order_relaxed);
          std::string sim_resp;
          double lat = MeasureMs([&]() {
            sim_resp =
                client.SendCommand("SIM " + viral_video + " 10 using=fusion");
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
          ? (static_cast<double>(b_sim_ok) / static_cast<double>(b_sim_total) * 100.0)
          : 0.0;

  std::cout << "\n  --- Burst Results ---\n";
  std::cout << "  Burst duration:           " << std::fixed << std::setprecision(1)
            << burst_ms << " ms\n";
  std::cout << "  Events recorded:          " << b_events << "/" << kBurstViewers
            << "\n";
  std::cout << "  SIM success rate:         " << burst_rate << "% (" << b_sim_ok
            << "/" << b_sim_total << ")\n";
  PrintLatency("Burst SIM", burst_latencies);

  // Compare normal vs burst latency.
  if (!normal_latencies.empty() && !burst_latencies.empty()) {
    std::sort(normal_latencies.begin(), normal_latencies.end());
    std::sort(burst_latencies.begin(), burst_latencies.end());
    double normal_p50 = Percentile(normal_latencies, 50);
    double burst_p50 = Percentile(burst_latencies, 50);
    double degradation = (normal_p50 > 0) ? (burst_p50 / normal_p50) : 0.0;
    std::cout << "\n  Latency degradation (p50): " << std::setprecision(1)
              << degradation << "x (normal=" << std::setprecision(3)
              << normal_p50 << " ms, burst=" << burst_p50 << " ms)\n";
  }

  // Expect the system to handle burst without catastrophic failure.
  EXPECT_GT(burst_rate, 80.0) << "Burst SIM success rate too low";
  EXPECT_GT(b_events, static_cast<int>(kBurstViewers * 80 / 100))
      << "Too many event failures during burst";
}

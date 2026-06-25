/**
 * @file nvecdclient_test.cpp
 * @brief Unit tests for nvecd C++ client library
 *
 * Reference: ../mygram-db/tests/client/mygramclient_test.cpp (patterns)
 * Note: This is an E2E integration test that requires a running nvecd server
 */

#include "client/nvecdclient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

#include "config/config.h"
#include "server/nvecd_server.h"

using namespace nvecd;
using namespace nvecd::client;
using namespace nvecd::utils;

namespace {

// Test server port (random high port to avoid conflicts)
constexpr uint16_t kTestPort = 0;  // Let OS assign random port

/**
 * @brief Test fixture with embedded server
 */
class NvecdClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create config
    config_ = std::make_unique<config::Config>();
    config_->api.tcp.port = kTestPort;  // Random port
    config_->api.http.enable = false;
    config_->perf.thread_pool_size = 2;                   // NOLINT
    config_->events.ctx_buffer_size = 100;                // NOLINT
    config_->events.decay_interval_sec = 60;              // NOLINT
    config_->events.decay_alpha = 0.9;                    // NOLINT
    config_->vectors.default_dimension = 3;               // Small dimension for tests
    config_->network.allow_cidrs = {"127.0.0.1/32"};      // Allow localhost
    config_->snapshot.dir = "/tmp/nvecd_test_snapshots";  // Use temp dir for tests

    // Create and start server (server owns stores)
    server_ = std::make_unique<server::NvecdServer>(*config_);
    auto start_result = server_->Start();
    ASSERT_TRUE(start_result) << "Failed to start server: " << start_result.error().message();

    // Get actual port (if we used port 0)
    port_ = server_->GetPort();
    ASSERT_GT(port_, 0) << "Server port is invalid";

    // Small delay to ensure server is ready
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // NOLINT
  }

  void TearDown() override {
    if (server_) {
      server_->Stop();
    }
  }

  std::unique_ptr<config::Config> config_;
  std::unique_ptr<server::NvecdServer> server_;
  uint16_t port_ = 0;
};

}  // namespace

//
// Response framing tests (detail::IsResponseComplete)
//
// These exercise the length-aware framing that fixes truncated multi-line
// SIM/SIMV responses delivered across multiple TCP segments. They are pure
// function tests and do not require a running server.
//

TEST(NvecdClientFramingTest, SingleLineOkCompleteOnNewline) {
  EXPECT_FALSE(detail::IsResponseComplete("OK"));
  EXPECT_TRUE(detail::IsResponseComplete("OK\r\n"));
  EXPECT_TRUE(detail::IsResponseComplete("OK\n"));
}

TEST(NvecdClientFramingTest, SingleLineErrorCompleteOnNewline) {
  EXPECT_FALSE(detail::IsResponseComplete("ERROR Item not found"));
  EXPECT_TRUE(detail::IsResponseComplete("ERROR Item not found\r\n"));
}

TEST(NvecdClientFramingTest, ResultsHeaderAloneIsIncomplete) {
  // The header arrived (and ends in \r\n) but no result rows have been read yet.
  // The old "ends with \r\n" heuristic incorrectly treated this as complete.
  EXPECT_FALSE(detail::IsResponseComplete("OK RESULTS 3\r\n"));
}

TEST(NvecdClientFramingTest, ResultsPartialRowsIsIncomplete) {
  EXPECT_FALSE(detail::IsResponseComplete("OK RESULTS 3\r\nitem789 0.9245\r\n"));
  EXPECT_FALSE(detail::IsResponseComplete("OK RESULTS 3\r\nitem789 0.9245\r\nitem101 0.8932\r\n"));
}

TEST(NvecdClientFramingTest, ResultsAllRowsIsComplete) {
  EXPECT_TRUE(detail::IsResponseComplete("OK RESULTS 3\r\nitem789 0.9245\r\nitem101 0.8932\r\nitem202 0.8567\r\n"));
}

TEST(NvecdClientFramingTest, ResultsZeroCountCompleteAfterHeader) {
  EXPECT_TRUE(detail::IsResponseComplete("OK RESULTS 0\r\n"));
}

TEST(NvecdClientFramingTest, ChunkedFeedCompletesOnlyWhenAllRowsArrive) {
  // Simulate a response delivered across TCP segments: the header arrives first
  // (ending in \r\n), then each result row arrives separately.
  const std::vector<std::string> chunks = {"OK RESULTS 3\r\n", "item789 0.9245\r\n", "item101 0.8932\r\n",
                                           "item202 0.8567\r\n"};
  std::string buffer;
  for (size_t i = 0; i < chunks.size(); ++i) {
    buffer += chunks[i];
    bool is_last = (i + 1 == chunks.size());
    EXPECT_EQ(detail::IsResponseComplete(buffer), is_last) << "after chunk " << i;
  }
}

TEST(NvecdClientFramingTest, MalformedCountTreatedAsComplete) {
  // A non-numeric count must not block the read loop forever; the parser layer
  // will then surface a protocol error.
  EXPECT_TRUE(detail::IsResponseComplete("OK RESULTS abc\r\n"));
}

//
// Connection tests
//

TEST_F(NvecdClientTest, ConnectSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  auto result = client.Connect();
  ASSERT_TRUE(result) << "Connect failed: " << result.error().message();
  EXPECT_TRUE(client.IsConnected());

  client.Disconnect();
  EXPECT_FALSE(client.IsConnected());
}

TEST_F(NvecdClientTest, ConnectInvalidPort) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = 1;  // Invalid port

  NvecdClient client(config);
  auto result = client.Connect();
  EXPECT_FALSE(result);
  EXPECT_FALSE(client.IsConnected());
}

//
// EVENT command tests
//

TEST_F(NvecdClientTest, EventSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto result = client.Event("ctx123", "ADD", "vec456", 95);  // NOLINT
  EXPECT_TRUE(result) << "Event failed: " << result.error().message();
}

TEST_F(NvecdClientTest, EventInvalidScore) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto result = client.Event("ctx123", "ADD", "vec456", 150);  // NOLINT - invalid score
  EXPECT_FALSE(result);
}

//
// VECSET command tests
//

TEST_F(NvecdClientTest, VecsetSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  std::vector<float> vec = {0.1F, 0.2F, 0.3F};
  auto result = client.Vecset("vec1", vec);
  EXPECT_TRUE(result) << "Vecset failed: " << result.error().message();
}

TEST_F(NvecdClientTest, VecsetEmptyVector) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  std::vector<float> vec;
  auto result = client.Vecset("vec1", vec);
  EXPECT_FALSE(result);
}

//
// SIM command tests
//

TEST_F(NvecdClientTest, SimSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  // Register vectors
  std::vector<float> vec1 = {1.0F, 0.0F, 0.0F};
  std::vector<float> vec2 = {0.9F, 0.1F, 0.0F};  // NOLINT
  auto vec1_result = client.Vecset("vec1", vec1);
  ASSERT_TRUE(vec1_result) << "Vecset vec1 failed: " << vec1_result.error().message();

  auto vec2_result = client.Vecset("vec2", vec2);
  ASSERT_TRUE(vec2_result) << "Vecset vec2 failed: " << vec2_result.error().message();

  // Search
  auto result = client.Sim("vec1", 10, "vectors");  // NOLINT
  ASSERT_TRUE(result) << "Sim failed: " << result.error().message();

  EXPECT_EQ(result->mode, "vectors");
  EXPECT_GE(result->results.size(), 1) << "Expected at least vec2 in results";
  if (!result->results.empty()) {
    EXPECT_EQ(result->results[0].id, "vec2");  // vec2 should be most similar (self is skipped)
  }
}

TEST_F(NvecdClientTest, SimNonExistentId) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto result = client.Sim("nonexistent", 10, "vectors");  // NOLINT
  EXPECT_FALSE(result);                                    // Should fail for non-existent ID
}

//
// SIMV command tests
//

TEST_F(NvecdClientTest, SimvSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  // Register vectors
  std::vector<float> vec1 = {1.0F, 0.0F, 0.0F};
  auto vec1_result = client.Vecset("vec1", vec1);
  ASSERT_TRUE(vec1_result) << "Vecset vec1 failed: " << vec1_result.error().message();

  // Search by vector
  std::vector<float> query = {0.9F, 0.1F, 0.0F};    // NOLINT
  auto result = client.Simv(query, 10, "vectors");  // NOLINT
  ASSERT_TRUE(result) << "Simv failed: " << result.error().message();

  EXPECT_EQ(result->mode, "vectors");
  EXPECT_GE(result->results.size(), 1);
  if (!result->results.empty()) {
    EXPECT_EQ(result->results[0].id, "vec1");
  }
}

//
// METASET command tests
//

TEST_F(NvecdClientTest, MetasetSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  // The item must exist before metadata can be attached.
  std::vector<float> vec = {0.1F, 0.2F, 0.3F};
  ASSERT_TRUE(client.Vecset("item1", vec));

  auto result = client.Metaset("item1", "category:electronics,active:true");
  EXPECT_TRUE(result) << "Metaset failed: " << result.error().message();
}

TEST_F(NvecdClientTest, MetasetUnknownItemFails) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  // No VECSET for this ID — server must reject the metadata.
  auto result = client.Metaset("ghost", "category:electronics");
  EXPECT_FALSE(result);
}

TEST_F(NvecdClientTest, MetasetEnablesFilteredSim) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  std::vector<float> vec1 = {1.0F, 0.0F, 0.0F};
  std::vector<float> vec2 = {0.9F, 0.1F, 0.0F};  // NOLINT
  ASSERT_TRUE(client.Vecset("vec1", vec1));
  ASSERT_TRUE(client.Vecset("vec2", vec2));
  ASSERT_TRUE(client.Metaset("vec2", "category:books"));

  // Filter to a category vec2 does not have — vec2 must be excluded.
  SearchOptions options;
  options.filter = "category:music";
  auto result = client.Sim("vec1", 10, "vectors", options);  // NOLINT
  ASSERT_TRUE(result) << "Sim failed: " << result.error().message();
  for (const auto& item : result->results) {
    EXPECT_NE(item.id, "vec2") << "vec2 should be filtered out by category:music";
  }
}

//
// AUTH command tests
//

TEST_F(NvecdClientTest, AuthNoPasswordConfigured) {
  // The test server has no requirepass set, so AUTH returns "+OK (no password
  // required)". The client must accept the Redis-style "+OK" prefix.
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto result = client.Auth("anything");
  EXPECT_TRUE(result) << "Auth failed: " << result.error().message();
}

//
// CACHE command tests
//

TEST_F(NvecdClientTest, CacheCommands) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto stats = client.CacheStats();
  ASSERT_TRUE(stats) << "CacheStats failed: " << stats.error().message();
  EXPECT_NE(stats->find("CACHE_STATS"), std::string::npos);

  EXPECT_TRUE(client.CacheClear()) << "CacheClear failed";
  EXPECT_TRUE(client.CacheEnable()) << "CacheEnable failed";
  EXPECT_TRUE(client.CacheDisable()) << "CacheDisable failed";
}

//
// DUMP STATUS command tests
//

TEST_F(NvecdClientTest, DumpStatusSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto result = client.DumpStatus();
  ASSERT_TRUE(result) << "DumpStatus failed: " << result.error().message();
  EXPECT_NE(result->find("status:"), std::string::npos);
}

//
// SIMV options / float precision tests
//

TEST_F(NvecdClientTest, SimvWithMinScoreOption) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  std::vector<float> vec1 = {1.0F, 0.0F, 0.0F};
  std::vector<float> vec2 = {0.0F, 1.0F, 0.0F};  // Orthogonal to the query
  ASSERT_TRUE(client.Vecset("vec1", vec1));
  ASSERT_TRUE(client.Vecset("vec2", vec2));

  // A high min_score must exclude the orthogonal/low-score item. This only
  // works if the client actually serialized "min_score=" into the SIMV command.
  std::vector<float> query = {1.0F, 0.0F, 0.0F};
  SearchOptions options;
  options.min_score = 0.99F;                                 // NOLINT
  auto result = client.Simv(query, 10, "vectors", options);  // NOLINT
  ASSERT_TRUE(result) << "Simv failed: " << result.error().message();
  for (const auto& item : result->results) {
    EXPECT_GE(item.score, 0.99F) << "min_score filter was not applied server-side";
    EXPECT_NE(item.id, "vec2") << "orthogonal vec2 should be excluded by min_score";
  }
}

TEST_F(NvecdClientTest, FloatPrecisionRoundTrips) {
  // A value whose 9th significant digit matters: 6-digit serialization would
  // perturb the stored vector. With round-trippable precision, a self-query
  // against the identical stored vector yields a near-perfect score.
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  std::vector<float> precise = {0.123456789F, 0.987654321F, 0.135792468F};
  ASSERT_TRUE(client.Vecset("precise1", precise));

  // Query with the exact same components. If serialization round-trips, the
  // normalized self-similarity is ~1.0.
  auto result = client.Simv(precise, 5, "vectors");  // NOLINT
  ASSERT_TRUE(result) << "Simv failed: " << result.error().message();
  ASSERT_FALSE(result->results.empty());
  bool found = false;
  for (const auto& item : result->results) {
    if (item.id == "precise1") {
      found = true;
      EXPECT_NEAR(item.score, 1.0F, 1e-4F) << "stored vector was perturbed during serialization";
    }
  }
  EXPECT_TRUE(found) << "precise1 should be returned for its own query vector";
}

//
// INFO command tests
//

TEST_F(NvecdClientTest, InfoSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto result = client.Info();
  ASSERT_TRUE(result) << "Info failed: " << result.error().message();

  EXPECT_FALSE(result->version.empty());
  EXPECT_GE(result->uptime_seconds, 0);
}

TEST_F(NvecdClientTest, InfoParsesRealKeysToNonZero) {
  // Regression for the key-name drift: the client previously read keys the
  // server never emits (total_requests / co_occurrence_entries), leaving the
  // fields at 0. Drive some traffic and confirm the correctly named fields are
  // populated from the server's actual INFO output.
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  // Generate commands and an indexed item so id_count/command counters move.
  std::vector<float> vec = {0.5F, 0.5F, 0.5F};
  ASSERT_TRUE(client.Vecset("metric_item", vec));
  // Two distinct items in one context register a co-occurrence pair, so
  // id_count becomes non-zero.
  ASSERT_TRUE(client.Event("ctxA", "ADD", "metric_item", 50));   // NOLINT
  ASSERT_TRUE(client.Event("ctxA", "ADD", "metric_item2", 60));  // NOLINT

  auto result = client.Info();
  ASSERT_TRUE(result) << "Info failed: " << result.error().message();

  EXPECT_GT(result->total_commands_processed, 0U) << "total_commands_processed should be parsed from INFO";
  EXPECT_GT(result->vector_count, 0U) << "vector_count should reflect the registered vector";
  EXPECT_GT(result->id_count, 0U) << "id_count should reflect co-occurrence IDs";
}

//
// CONFIG command tests
//

TEST_F(NvecdClientTest, GetConfigSuccess) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto result = client.GetConfig();
  ASSERT_TRUE(result) << "GetConfig failed: " << result.error().message();
  EXPECT_FALSE(result->empty());
}

//
// DEBUG command tests
//

TEST_F(NvecdClientTest, DebugCommands) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  auto enable_result = client.EnableDebug();
  EXPECT_TRUE(enable_result) << "EnableDebug failed: " << enable_result.error().message();

  auto disable_result = client.DisableDebug();
  EXPECT_TRUE(disable_result) << "DisableDebug failed: " << disable_result.error().message();
}

//
// DUMP command tests (basic smoke tests)
//

TEST_F(NvecdClientTest, DumpCommandsBasic) {
  ClientConfig config;
  config.host = "127.0.0.1";
  config.port = port_;

  NvecdClient client(config);
  ASSERT_TRUE(client.Connect());

  // Save with default filename
  auto save_result = client.Save("");
  ASSERT_TRUE(save_result) << "Save failed: " << save_result.error().message();
  EXPECT_FALSE(save_result->empty());
}

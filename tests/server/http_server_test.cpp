/**
 * @file http_server_test.cpp
 * @brief Unit tests for HTTP server
 *
 * Tests HTTP endpoints:
 * - Health endpoints
 * - Info/Config endpoints
 * - Metrics endpoint
 * - nvecd-specific endpoints (EVENT, VECSET, SIM, SIMV)
 * - Cache management endpoints
 */

#include "server/http_server.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <httplib.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "cache/similarity_cache.h"
#include "cache/similarity_cache_controller.h"
#include "config/config.h"
#include "config/runtime_variable_manager.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/server_types.h"
#include "similarity/similarity_engine.h"
#include "storage/wal.h"
#include "vectors/metadata_store.h"
#include "vectors/vector_store.h"

using json = nlohmann::json;
using namespace nvecd;

namespace {

// Test fixture with HTTP server and components
class HttpServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create config
    config_ = std::make_unique<config::Config>();
    config_->events.ctx_buffer_size = 50;
    config_->vectors.default_dimension = 4;
    config_->similarity.default_top_k = 10;
    config_->similarity.fusion_alpha = 0.6f;
    config_->similarity.fusion_beta = 0.4f;
    config_->cache.enabled = true;

    // Initialize components
    event_store_ = std::make_unique<events::EventStore>(config_->events);
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    vector_store_ = std::make_unique<vectors::VectorStore>(config_->vectors);
    metadata_store_ = std::make_unique<vectors::MetadataStore>();
    similarity_engine_ =
        std::make_unique<similarity::SimilarityEngine>(event_store_.get(), co_index_.get(), vector_store_.get(),
                                                       config_->similarity, config_->vectors, metadata_store_.get());
    auto manager_result = config::RuntimeVariableManager::Create(*config_);
    ASSERT_TRUE(manager_result);
    variable_manager_ = std::move(*manager_result);
    cache_controller_ = std::make_unique<cache::SimilarityCacheController>(10 * 1024 * 1024, 0.1, 0, true, 1, true,
                                                                           &handler_ctx_.cache);
    cache_ = &cache_controller_->Cache();
    variable_manager_->SetCacheController(cache_controller_.get());

    // Setup handler context (pointers only, references already set in initializer)
    handler_ctx_.event_store = event_store_.get();
    handler_ctx_.co_index = co_index_.get();
    handler_ctx_.vector_store = vector_store_.get();
    handler_ctx_.metadata_store = metadata_store_.get();
    handler_ctx_.similarity_engine = similarity_engine_.get();
    handler_ctx_.cache_controller = cache_controller_.get();
    handler_ctx_.variable_manager = variable_manager_.get();
    handler_ctx_.config = config_.get();
    handler_ctx_.write_serialization_gate = &write_serialization_gate_;

    // Create HTTP server. Port 0 lets the OS pick a free port, so the suite
    // never collides with whatever else happens to be listening on the host.
    server::HttpServerConfig http_config;
    http_config.bind = "127.0.0.1";
    http_config.port = 0;
    http_config.allow_cidrs = {"127.0.0.0/8"};
    ConfigureHttpServer(&http_config);

    http_server_ = std::make_unique<server::HttpServer>(http_config, &handler_ctx_, config_.get(), &loading_, &stats_);

    // Start server
    auto result = http_server_->Start();
    ASSERT_TRUE(result) << "Failed to start HTTP server: " << result.error().message();

    port_ = http_server_->GetPort();
    ASSERT_GT(port_, 0) << "HTTP server did not report a bound port";

    // Wait for server to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Create HTTP client
    client_ = std::make_unique<httplib::Client>(BaseUrl());
  }

  void TearDown() override {
    client_.reset();
    if (http_server_) {
      http_server_->Stop();
    }
    cache_controller_.reset();
  }

  virtual void ConfigureHttpServer(server::HttpServerConfig* /* config */) {}

  /// Base URL of the server on the port the OS assigned.
  std::string BaseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

  std::unique_ptr<config::Config> config_;
  std::unique_ptr<events::EventStore> event_store_;
  std::unique_ptr<events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<vectors::VectorStore> vector_store_;
  std::unique_ptr<vectors::MetadataStore> metadata_store_;
  std::unique_ptr<similarity::SimilarityEngine> similarity_engine_;
  cache::SimilarityCache* cache_ = nullptr;
  std::unique_ptr<cache::SimilarityCacheController> cache_controller_;
  std::unique_ptr<config::RuntimeVariableManager> variable_manager_;

  server::ServerStats stats_;
  std::atomic<bool> loading_{false};
  std::atomic<bool> read_only_{false};
  std::mutex write_serialization_gate_;
  server::HandlerContext handler_ctx_{
      .event_store = nullptr,
      .co_index = nullptr,
      .vector_store = nullptr,
      .metadata_store = nullptr,
      .similarity_engine = nullptr,
      .cache = nullptr,
      .stats = stats_,
      .config = nullptr,
      .loading = loading_,
      .read_only = read_only_,
      .dump_dir = "",
  };

  std::unique_ptr<server::HttpServer> http_server_;
  std::unique_ptr<httplib::Client> client_;
  int port_ = 0;
};

// Health endpoint tests
TEST_F(HttpServerTest, HealthLive) {
  auto res = client_->Get("/health/live");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "alive");
  EXPECT_TRUE(body.contains("timestamp"));
}

TEST_F(HttpServerTest, HealthReady) {
  auto res = client_->Get("/health/ready");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ready");
  EXPECT_EQ(body["loading"], false);
}

TEST_F(HttpServerTest, HealthReadyWhileLoading) {
  loading_.store(true);

  auto res = client_->Get("/health/ready");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 503);

  auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "not_ready");
  EXPECT_EQ(body["loading"], true);

  loading_.store(false);
}

TEST_F(HttpServerTest, MutationIsRejectedWhileLoading) {
  loading_.store(true, std::memory_order_release);

  json body;
  body["id"] = "blocked";
  body["vector"] = {1.0, 0.0, 0.0, 0.0};
  auto res = client_->Post("/vecset", body.dump(), "application/json");

  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 503);
  EXPECT_FALSE(vector_store_->HasVector("blocked"));
  loading_.store(false, std::memory_order_release);
}

TEST_F(HttpServerTest, WalAppendFailureReturns503WithoutMutation) {
  storage::WriteAheadLog closed_wal;
  handler_ctx_.wal = &closed_wal;
  json body;
  body["id"] = "not-accepted";
  body["vector"] = {1.0, 0.0, 0.0, 0.0};

  auto res = client_->Post("/vecset", body.dump(), "application/json");

  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 503);
  EXPECT_FALSE(vector_store_->HasVector("not-accepted"));
  EXPECT_TRUE(read_only_.load(std::memory_order_acquire));
}

TEST_F(HttpServerTest, HealthDetail) {
  auto res = client_->Get("/health/detail");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "healthy");
  EXPECT_TRUE(body.contains("components"));
  EXPECT_TRUE(body["components"].contains("event_store"));
  EXPECT_TRUE(body["components"].contains("vector_store"));
  EXPECT_TRUE(body["components"].contains("co_index"));
}

TEST_F(HttpServerTest, HealthDetail_ReturnsRealUptime) {
  std::this_thread::sleep_for(std::chrono::seconds(1));
  auto res = client_->Get("/health/detail");
  ASSERT_TRUE(res);
  auto body = json::parse(res->body);
  EXPECT_GT(body["uptime_seconds"].get<uint64_t>(), 0);
}

// Info and Config tests
TEST_F(HttpServerTest, Info) {
  auto res = client_->Get("/info");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  EXPECT_EQ(body["server"], "nvecd");
  EXPECT_TRUE(body.contains("version"));
  EXPECT_TRUE(body.contains("memory"));
  EXPECT_TRUE(body.contains("stores"));
}

TEST_F(HttpServerTest, Info_ReturnsRealUptime) {
  // uptime_seconds is reported as whole seconds, so wait comfortably past the
  // integer-second boundary to avoid flaking when the elapsed time rounds down
  // to 0 under load.
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  auto res = client_->Get("/info");
  ASSERT_TRUE(res);
  auto body = json::parse(res->body);
  EXPECT_GT(body["uptime_seconds"].get<uint64_t>(), 0);
}

TEST_F(HttpServerTest, Info_ReturnsRealPeakMemory) {
  auto res = client_->Get("/info");
  ASSERT_TRUE(res);
  auto body = json::parse(res->body);
  EXPECT_GT(body["memory"]["peak_memory_bytes"].get<uint64_t>(), 0);
}

TEST_F(HttpServerTest, Config) {
  auto res = client_->Get("/config");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("network"));
  EXPECT_TRUE(body.contains("events"));
  EXPECT_TRUE(body.contains("vectors"));
  EXPECT_TRUE(body.contains("similarity"));
}

TEST_F(HttpServerTest, StatelessDebugEndpointsAreExplicitlyUnsupported) {
  auto enabled = client_->Post("/debug/on", "", "application/json");
  ASSERT_TRUE(enabled);
  EXPECT_EQ(enabled->status, 410);

  auto disabled = client_->Post("/debug/off", "", "application/json");
  ASSERT_TRUE(disabled);
  EXPECT_EQ(disabled->status, 410);
}

// Metrics test
TEST_F(HttpServerTest, Metrics) {
  auto res = client_->Get("/metrics");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(res->get_header_value("Content-Type"), "text/plain; version=0.0.4; charset=utf-8");

  // Check for Prometheus format
  EXPECT_TRUE(res->body.find("# HELP nvecd_uptime_seconds") != std::string::npos);
  EXPECT_TRUE(res->body.find("# TYPE nvecd_uptime_seconds counter") != std::string::npos);
  EXPECT_TRUE(res->body.find("nvecd_commands_total") != std::string::npos);
  EXPECT_TRUE(res->body.find("nvecd_memory_bytes") != std::string::npos);
}

TEST_F(HttpServerTest, Metrics_ReturnsRealUptime) {
  std::this_thread::sleep_for(std::chrono::seconds(1));
  auto res = client_->Get("/metrics");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->body.find("nvecd_uptime_seconds 0"), std::string::npos);
}

// Vector operations tests
TEST_F(HttpServerTest, Vecset) {
  json req_body;
  req_body["id"] = "test1";
  req_body["vector"] = {0.1f, 0.9f, 0.2f, 0.5f};

  auto res = client_->Post("/vecset", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ok");
  EXPECT_EQ(body["dimension"], 4);
}

TEST_F(HttpServerTest, VecsetInvalidJSON) {
  auto res = client_->Post("/vecset", "invalid json", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);

  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(HttpServerTest, VecsetMissingFields) {
  json req_body;
  req_body["id"] = "test1";
  // Missing "vector" field

  auto res = client_->Post("/vecset", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);

  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(HttpServerTest, VecdelRemovesVectorAndMetadata) {
  auto vecset = client_->Post("/vecset", R"({"id":"delete_me","vector":[1,0,0,0]})", "application/json");
  ASSERT_TRUE(vecset);
  ASSERT_EQ(vecset->status, 200);
  auto metaset = client_->Post("/metaset", R"({"id":"delete_me","metadata":{"state":"active"}})", "application/json");
  ASSERT_TRUE(metaset);
  ASSERT_EQ(metaset->status, 200);

  auto deleted = client_->Delete("/vecset", R"({"id":"delete_me"})", "application/json");
  ASSERT_TRUE(deleted);
  EXPECT_EQ(deleted->status, 200);
  EXPECT_FALSE(vector_store_->HasVector("delete_me"));
  EXPECT_EQ(metadata_store_->Get("delete_me"), nullptr);

  auto missing = client_->Delete("/vecset", R"({"id":"delete_me"})", "application/json");
  ASSERT_TRUE(missing);
  EXPECT_EQ(missing->status, 404);
}

// Event operations tests
TEST_F(HttpServerTest, Event) {
  json req_body;
  req_body["ctx"] = "user123";
  req_body["type"] = "ADD";
  req_body["id"] = "item456";
  req_body["score"] = 10;

  auto res = client_->Post("/event", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ok");
}

TEST_F(HttpServerTest, EventMissingFields) {
  json req_body;
  req_body["ctx"] = "user123";
  // Missing "id" and "score"

  auto res = client_->Post("/event", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

// Similarity search tests
TEST_F(HttpServerTest, Simv) {
  // First, add some vectors
  json vec1;
  vec1["id"] = "test1";
  vec1["vector"] = {0.1f, 0.9f, 0.2f, 0.5f};
  client_->Post("/vecset", vec1.dump(), "application/json");

  json vec2;
  vec2["id"] = "test2";
  vec2["vector"] = {0.9f, 0.1f, 0.8f, 0.3f};
  client_->Post("/vecset", vec2.dump(), "application/json");

  // Search by vector
  json req_body;
  req_body["vector"] = {0.1f, 0.9f, 0.2f, 0.5f};
  req_body["top_k"] = 5;

  auto res = client_->Post("/simv", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ok");
  EXPECT_TRUE(body.contains("results"));
  EXPECT_GT(body["count"], 0);
  EXPECT_EQ(body["dimension"], 4);
}

TEST_F(HttpServerTest, Sim) {
  // First, add vectors
  json vec1;
  vec1["id"] = "test1";
  vec1["vector"] = {0.1f, 0.9f, 0.2f, 0.5f};
  client_->Post("/vecset", vec1.dump(), "application/json");

  json vec2;
  vec2["id"] = "test2";
  vec2["vector"] = {0.9f, 0.1f, 0.8f, 0.3f};
  client_->Post("/vecset", vec2.dump(), "application/json");

  // Search by ID
  json req_body;
  req_body["id"] = "test1";
  req_body["top_k"] = 5;
  req_body["mode"] = "vectors";

  auto res = client_->Post("/sim", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ok");
  EXPECT_EQ(body["mode"], "vectors");
  EXPECT_TRUE(body.contains("results"));
}

TEST_F(HttpServerTest, SimWithMetadataFilter) {
  json vec1;
  vec1["id"] = "active_item";
  vec1["vector"] = {1.0f, 0.0f, 0.0f, 0.0f};
  vec1["metadata"] = {{"status", "active"}, {"category", "electronics"}};
  ASSERT_EQ(client_->Post("/vecset", vec1.dump(), "application/json")->status, 200);

  json vec2;
  vec2["id"] = "draft_item";
  vec2["vector"] = {0.9f, 0.1f, 0.0f, 0.0f};
  vec2["metadata"] = {{"status", "draft"}, {"category", "electronics"}};
  ASSERT_EQ(client_->Post("/vecset", vec2.dump(), "application/json")->status, 200);

  json req_body;
  req_body["id"] = "active_item";
  req_body["top_k"] = 5;
  req_body["mode"] = "vectors";
  req_body["filter"] = "status:draft";

  auto res = client_->Post("/sim", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  ASSERT_EQ(body["count"], 1);
  EXPECT_EQ(body["results"][0]["id"], "draft_item");
}

TEST_F(HttpServerTest, SimvWithMetadataFilterAndMinScore) {
  json vec1;
  vec1["id"] = "active_item";
  vec1["vector"] = {1.0f, 0.0f, 0.0f, 0.0f};
  vec1["metadata"] = {{"status", "active"}};
  ASSERT_EQ(client_->Post("/vecset", vec1.dump(), "application/json")->status, 200);

  json vec2;
  vec2["id"] = "draft_item";
  vec2["vector"] = {0.9f, 0.1f, 0.0f, 0.0f};
  vec2["metadata"] = {{"status", "draft"}};
  ASSERT_EQ(client_->Post("/vecset", vec2.dump(), "application/json")->status, 200);

  json req_body;
  req_body["vector"] = {1.0f, 0.0f, 0.0f, 0.0f};
  req_body["top_k"] = 5;
  req_body["filter"] = "status:draft";
  req_body["min_score"] = 0.1;

  auto res = client_->Post("/simv", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  ASSERT_EQ(body["count"], 1);
  EXPECT_EQ(body["results"][0]["id"], "draft_item");
}

TEST_F(HttpServerTest, SimNotFound) {
  json req_body;
  req_body["id"] = "nonexistent";
  req_body["top_k"] = 5;
  req_body["mode"] = "vectors";

  auto res = client_->Post("/sim", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(HttpServerTest, SimAndSimvRejectWrongOptionalFieldTypesWithBadRequest) {
  json sim_body = {{"id", "item"}, {"top_k", "five"}, {"mode", true}};
  auto sim_response = client_->Post("/sim", sim_body.dump(), "application/json");
  ASSERT_TRUE(sim_response);
  EXPECT_EQ(sim_response->status, 400);

  json simv_body = {{"vector", json::array({1.0, 0.0, 0.0, 0.0})}, {"top_k", "five"}};
  auto simv_response = client_->Post("/simv", simv_body.dump(), "application/json");
  ASSERT_TRUE(simv_response);
  EXPECT_EQ(simv_response->status, 400);

  json invalid_top_k = {{"vector", json::array({1.0, 0.0, 0.0, 0.0})}, {"top_k", -1}};
  auto invalid_top_k_response = client_->Post("/simv", invalid_top_k.dump(), "application/json");
  ASSERT_TRUE(invalid_top_k_response);
  EXPECT_EQ(invalid_top_k_response->status, 400);
}

TEST_F(HttpServerTest, BindFailureIsReportedSynchronouslyAndStopStillJoinsSafely) {
  server::HttpServerConfig duplicate_config;
  duplicate_config.bind = "invalid-bind-address";
  // The bind fails while resolving the address, so no port is ever claimed;
  // 0 keeps the test from depending on a fixed port being free anyway.
  duplicate_config.port = 0;
  duplicate_config.allow_cidrs = {"127.0.0.0/8"};
  server::HttpServer duplicate(duplicate_config, &handler_ctx_, config_.get(), &loading_, &stats_);

  auto started = duplicate.Start();
  ASSERT_FALSE(started.has_value());
  // A failure to resolve the address is not a port conflict, and must not be
  // reported as one.
  EXPECT_EQ(started.error().code(), utils::ErrorCode::kNetworkBindFailed) << started.error().to_string();
  duplicate.Stop();
  EXPECT_FALSE(duplicate.IsRunning());
}

TEST_F(HttpServerTest, SecondServerOnTheSamePortIsRefused) {
  // The fixture's server already holds port_. A second listener on the same
  // address must be refused rather than admitted alongside it: two servers
  // sharing one port would have the kernel hand each connection to whichever
  // instance it picked, so clients would see answers from two different
  // datasets with nothing logged.
  server::HttpServerConfig duplicate_config;
  duplicate_config.bind = "127.0.0.1";
  duplicate_config.port = port_;
  duplicate_config.allow_cidrs = {"127.0.0.0/8"};
  server::HttpServer duplicate(duplicate_config, &handler_ctx_, config_.get(), &loading_, &stats_);

  auto started = duplicate.Start();
  ASSERT_FALSE(started.has_value()) << "Second server bound port " << port_ << " already held by the first";
  EXPECT_EQ(started.error().code(), utils::ErrorCode::kNetworkAddressInUse) << started.error().to_string();
  EXPECT_NE(started.error().message().find("another server is listening"), std::string::npos)
      << "Bind conflict was not reported as such: " << started.error().message();
  EXPECT_FALSE(duplicate.IsRunning());

  // The original server must still be the one serving the port.
  auto res = client_->Get("/health/live");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

TEST_F(HttpServerTest, RestartOnTheSamePortSucceedsWhileEarlierSocketsLinger) {
  // Drive a connection the server itself closes, so the accepted socket is
  // still lingering on the port when the listener is rebound the way an
  // operator restart would. SO_REUSEADDR is what lets that bind succeed; the
  // duplicate-bind refusal above must not come at the cost of every restart.
  const httplib::Headers close_connection = {{"Connection", "close"}};
  auto res = client_->Get("/health/live", close_connection);
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200);
  client_.reset();
  http_server_->Stop();

  server::HttpServerConfig restart_config;
  restart_config.bind = "127.0.0.1";
  restart_config.port = port_;
  restart_config.allow_cidrs = {"127.0.0.0/8"};
  server::HttpServer restarted(restart_config, &handler_ctx_, config_.get(), &loading_, &stats_);

  auto started = restarted.Start();
  ASSERT_TRUE(started) << "Restart on port " << port_ << " failed: " << started.error().message();
  EXPECT_EQ(restarted.GetPort(), port_);

  httplib::Client reconnected("http://127.0.0.1:" + std::to_string(port_));
  auto after_restart = reconnected.Get("/health/live");
  ASSERT_TRUE(after_restart);
  EXPECT_EQ(after_restart->status, 200);
  restarted.Stop();
}

// Cache management tests
TEST_F(HttpServerTest, CacheStats) {
  auto res = client_->Get("/cache/stats");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  EXPECT_EQ(body["enabled"], true);
  EXPECT_TRUE(body.contains("total_queries"));
  EXPECT_TRUE(body.contains("cache_hits"));
  EXPECT_TRUE(body.contains("cache_misses"));
  EXPECT_TRUE(body.contains("hit_rate"));
  EXPECT_TRUE(body.contains("current_entries"));
  EXPECT_TRUE(body.contains("current_memory_bytes"));
}

TEST_F(HttpServerTest, CacheClear) {
  json req_body;
  req_body["scope"] = "all";

  auto res = client_->Post("/cache/clear", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ok");
  EXPECT_EQ(body["scope"], "all");
  EXPECT_TRUE(body.contains("entries_removed"));
}

TEST_F(HttpServerTest, CacheClearInvalidScope) {
  json req_body;
  req_body["scope"] = "invalid";

  auto res = client_->Post("/cache/clear", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

TEST_F(HttpServerTest, CacheClearRejectsMalformedOrWrongTypedBodyWithoutClearing) {
  const auto entries_before = cache_->GetStatistics().current_entries;

  auto malformed = client_->Post("/cache/clear", "{", "application/json");
  ASSERT_TRUE(malformed);
  EXPECT_EQ(malformed->status, 400);
  EXPECT_EQ(cache_->GetStatistics().current_entries, entries_before);

  auto wrong_type = client_->Post("/cache/clear", R"({"scope":42})", "application/json");
  ASSERT_TRUE(wrong_type);
  EXPECT_EQ(wrong_type->status, 400);
  EXPECT_EQ(cache_->GetStatistics().current_entries, entries_before);
}

TEST_F(HttpServerTest, CacheControlSurfacesShareEffectiveStateAndTuning) {
  auto disabled = client_->Post("/cache/disable", "{}", "application/json");
  ASSERT_TRUE(disabled);
  ASSERT_EQ(disabled->status, 200);
  EXPECT_EQ(handler_ctx_.cache.load(), nullptr);
  EXPECT_EQ(*variable_manager_->GetVariable("cache.enabled"), "false");

  ASSERT_TRUE(variable_manager_->SetVariable("cache.ttl_seconds", "19"));
  ASSERT_TRUE(variable_manager_->SetVariable("cache.min_query_cost_ms", "2.5"));

  auto enabled = client_->Post("/cache/enable", "{}", "application/json");
  ASSERT_TRUE(enabled);
  ASSERT_EQ(enabled->status, 200);
  EXPECT_EQ(handler_ctx_.cache.load(), cache_);
  EXPECT_EQ(*variable_manager_->GetVariable("cache.enabled"), "true");

  auto stats_response = client_->Get("/cache/stats");
  ASSERT_TRUE(stats_response);
  ASSERT_EQ(stats_response->status, 200);
  const auto stats = json::parse(stats_response->body);
  EXPECT_TRUE(stats["enabled"].get<bool>());
  EXPECT_EQ(stats["ttl_seconds"], 19);
  EXPECT_DOUBLE_EQ(stats["min_query_cost_ms"].get<double>(), 2.5);
}

// Default SIM mode is fusion when the request omits "mode"
TEST_F(HttpServerTest, SimDefaultModeIsFusion) {
  json vec1;
  vec1["id"] = "test1";
  vec1["vector"] = {0.1f, 0.9f, 0.2f, 0.5f};
  ASSERT_EQ(client_->Post("/vecset", vec1.dump(), "application/json")->status, 200);

  json req_body;
  req_body["id"] = "test1";
  req_body["top_k"] = 5;
  // mode intentionally omitted

  auto res = client_->Post("/sim", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  EXPECT_EQ(body["mode"], "fusion");
}

// POST /metaset sets metadata that subsequently affects filtered search
TEST_F(HttpServerTest, MetasetAffectsFilteredSearch) {
  json vec1;
  vec1["id"] = "item_a";
  vec1["vector"] = {1.0f, 0.0f, 0.0f, 0.0f};
  ASSERT_EQ(client_->Post("/vecset", vec1.dump(), "application/json")->status, 200);

  json vec2;
  vec2["id"] = "item_b";
  vec2["vector"] = {0.9f, 0.1f, 0.0f, 0.0f};
  ASSERT_EQ(client_->Post("/vecset", vec2.dump(), "application/json")->status, 200);

  // Tag item_b via METASET
  json meta;
  meta["id"] = "item_b";
  meta["metadata"] = {{"status", "draft"}};
  auto meta_res = client_->Post("/metaset", meta.dump(), "application/json");
  ASSERT_TRUE(meta_res);
  ASSERT_EQ(meta_res->status, 200);

  // Filtered search should now return only item_b
  json req_body;
  req_body["id"] = "item_a";
  req_body["top_k"] = 5;
  req_body["mode"] = "vectors";
  req_body["filter"] = "status:draft";

  auto res = client_->Post("/sim", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  ASSERT_EQ(body["count"], 1);
  EXPECT_EQ(body["results"][0]["id"], "item_b");
}

// METASET on an unknown id returns 404
TEST_F(HttpServerTest, MetasetUnknownIdReturns404) {
  json meta;
  meta["id"] = "does_not_exist";
  meta["metadata"] = {{"status", "draft"}};
  auto res = client_->Post("/metaset", meta.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// /info exposes the documented cache block keys
TEST_F(HttpServerTest, InfoIncludesCacheBlock) {
  auto res = client_->Get("/info");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  ASSERT_TRUE(body.contains("cache"));
  EXPECT_TRUE(body["cache"].contains("hit_rate"));
  EXPECT_TRUE(body["cache"].contains("total_queries"));
  EXPECT_TRUE(body["cache"].contains("cache_hits"));
}

// HTTP SIM populates the query cache: a second identical query is a cache hit
TEST_F(HttpServerTest, SimPopulatesCache) {
  // Cache trivial queries too (the default min-cost threshold would skip them).
  cache_->SetMinQueryCost(0.0);

  json vec1;
  vec1["id"] = "c1";
  vec1["vector"] = {0.1f, 0.9f, 0.2f, 0.5f};
  ASSERT_EQ(client_->Post("/vecset", vec1.dump(), "application/json")->status, 200);
  json vec2;
  vec2["id"] = "c2";
  vec2["vector"] = {0.9f, 0.1f, 0.8f, 0.3f};
  ASSERT_EQ(client_->Post("/vecset", vec2.dump(), "application/json")->status, 200);

  json req_body;
  req_body["id"] = "c1";
  req_body["top_k"] = 5;
  req_body["mode"] = "vectors";

  uint64_t hits_before = cache_->GetStatistics().cache_hits;
  ASSERT_EQ(client_->Post("/sim", req_body.dump(), "application/json")->status, 200);
  // Second identical query should be served from cache.
  ASSERT_EQ(client_->Post("/sim", req_body.dump(), "application/json")->status, 200);

  uint64_t hits_after = cache_->GetStatistics().cache_hits;
  EXPECT_GT(hits_after, hits_before);
}

// A warmed HTTP SIMV result must be invalidated when HTTP itself adds a new
// vector. The generation bump is essential here because a brand-new ID is not
// present in the cached result and therefore cannot be evicted selectively.
TEST_F(HttpServerTest, VecsetInvalidatesWarmSimvCacheViaVectorGeneration) {
  cache_->SetMinQueryCost(0.0);
  cache::CachePolicy vector_policy;
  vector_policy.enabled = true;
  vector_policy.ttl_seconds = 0;
  cache_->SetSearchTypePolicy(cache::SearchType::kVectorSearch, vector_policy);

  json first_vector;
  first_vector["id"] = "existing";
  first_vector["vector"] = {0.0F, 1.0F, 0.0F, 0.0F};
  ASSERT_EQ(client_->Post("/vecset", first_vector.dump(), "application/json")->status, 200);

  json query;
  query["vector"] = {1.0F, 0.0F, 0.0F, 0.0F};
  query["top_k"] = 10;
  auto warm = client_->Post("/simv", query.dump(), "application/json");
  ASSERT_TRUE(warm);
  ASSERT_EQ(warm->status, 200);

  // Confirm the second request is served from the warmed cache before the
  // mutation, so this test specifically covers cache invalidation.
  const uint64_t hits_before = cache_->GetStatistics().cache_hits;
  ASSERT_EQ(client_->Post("/simv", query.dump(), "application/json")->status, 200);
  EXPECT_GT(cache_->GetStatistics().cache_hits, hits_before);

  json added_vector;
  added_vector["id"] = "new_match";
  added_vector["vector"] = {1.0F, 0.0F, 0.0F, 0.0F};
  ASSERT_EQ(client_->Post("/vecset", added_vector.dump(), "application/json")->status, 200);

  auto refreshed = client_->Post("/simv", query.dump(), "application/json");
  ASSERT_TRUE(refreshed);
  ASSERT_EQ(refreshed->status, 200);
  const auto response = json::parse(refreshed->body);
  ASSERT_TRUE(response.contains("results"));
  EXPECT_TRUE(std::any_of(response["results"].begin(), response["results"].end(),
                          [](const json& item) { return item["id"] == "new_match"; }));
}

TEST_F(HttpServerTest, MetadataCommitPreventsInflightFilteredResultReadmission) {
  cache_->SetMinQueryCost(0.0);
  json vector;
  vector["id"] = "active";
  vector["vector"] = {1.0F, 0.0F, 0.0F, 0.0F};
  vector["metadata"] = {{"status", "active"}};
  ASSERT_EQ(client_->Post("/vecset", vector.dump(), "application/json")->status, 200);

  json query_body;
  query_body["vector"] = {1.0F, 0.0F, 0.0F, 0.0F};
  query_body["top_k"] = 10;
  query_body["filter"] = "status:active";
  const auto queries_before = cache_->GetStatistics().total_queries;

  std::unique_lock mutation_guard(write_serialization_gate_);
  auto query = std::async(std::launch::async, [body = query_body.dump(), url = BaseUrl()] {
    httplib::Client client(url);
    auto response = client.Post("/simv", body, "application/json");
    return response ? response->status : -1;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (cache_->GetStatistics().total_queries == queries_before && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_GT(cache_->GetStatistics().total_queries, queries_before);

  metadata_store_->Set("active", {{"status", std::string("draft")}});
  handler_ctx_.metadata_generation.fetch_add(1, std::memory_order_acq_rel);
  cache_->Clear();
  mutation_guard.unlock();

  EXPECT_EQ(query.get(), 200);
  EXPECT_EQ(cache_->GetStatistics().current_entries, 0U);
}

// DUMP SAVE failure returns a non-2xx with a real error (not 200 ok)
TEST_F(HttpServerTest, DumpSaveFailureReturnsError) {
  // dump_dir points at a non-existent directory, so the save must fail.
  handler_ctx_.dump_dir = "/nonexistent_nvecd_dir_xyz";

  json req_body;  // empty filepath -> auto-generated name under dump_dir
  auto res = client_->Post("/dump/save", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_NE(res->status, 200);
  EXPECT_GE(res->status, 400);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("error"));
}

// Integration test: Full workflow
TEST_F(HttpServerTest, FullWorkflow) {
  // 1. Add vectors
  json vec1;
  vec1["id"] = "item1";
  vec1["vector"] = {0.1f, 0.9f, 0.2f, 0.5f};
  auto res1 = client_->Post("/vecset", vec1.dump(), "application/json");
  ASSERT_EQ(res1->status, 200);

  json vec2;
  vec2["id"] = "item2";
  vec2["vector"] = {0.9f, 0.1f, 0.8f, 0.3f};
  auto res2 = client_->Post("/vecset", vec2.dump(), "application/json");
  ASSERT_EQ(res2->status, 200);

  // 2. Add events
  json evt1;
  evt1["ctx"] = "user1";
  evt1["type"] = "ADD";
  evt1["id"] = "item1";
  evt1["score"] = 10;
  auto res3 = client_->Post("/event", evt1.dump(), "application/json");
  ASSERT_EQ(res3->status, 200);

  json evt2;
  evt2["ctx"] = "user1";
  evt2["type"] = "ADD";
  evt2["id"] = "item2";
  evt2["score"] = 5;
  auto res4 = client_->Post("/event", evt2.dump(), "application/json");
  ASSERT_EQ(res4->status, 200);

  // 3. Search by vector
  json simv_req;
  simv_req["vector"] = {0.1f, 0.9f, 0.2f, 0.5f};
  simv_req["top_k"] = 5;
  auto res5 = client_->Post("/simv", simv_req.dump(), "application/json");
  ASSERT_EQ(res5->status, 200);

  auto simv_body = json::parse(res5->body);
  EXPECT_GT(simv_body["count"], 0);

  // 4. Search by ID
  json sim_req;
  sim_req["id"] = "item1";
  sim_req["top_k"] = 5;
  sim_req["mode"] = "fusion";
  auto res6 = client_->Post("/sim", sim_req.dump(), "application/json");
  ASSERT_EQ(res6->status, 200);

  auto sim_body = json::parse(res6->body);
  EXPECT_EQ(sim_body["mode"], "fusion");

  // 5. Check metrics
  auto res7 = client_->Get("/metrics");
  ASSERT_EQ(res7->status, 200);
  EXPECT_TRUE(res7->body.find("nvecd_vectors_total 2") != std::string::npos);
  EXPECT_TRUE(res7->body.find("nvecd_events_total 2") != std::string::npos);

  // 6. Check info
  auto res8 = client_->Get("/info");
  ASSERT_EQ(res8->status, 200);
  auto info = json::parse(res8->body);
  EXPECT_EQ(info["stores"]["vector_store"]["vectors"], 2);
  EXPECT_EQ(info["stores"]["event_store"]["total_events"], 2);
}

TEST_F(HttpServerTest, SimScoresUseFixedFourDecimalPrecision) {
  json vec1;
  vec1["id"] = "self";
  vec1["vector"] = {1.0f, 0.0f, 0.0f, 0.0f};
  ASSERT_EQ(client_->Post("/vecset", vec1.dump(), "application/json")->status, 200);

  json vec2;
  vec2["id"] = "other";
  vec2["vector"] = {0.0f, 1.0f, 0.0f, 0.0f};
  ASSERT_EQ(client_->Post("/vecset", vec2.dump(), "application/json")->status, 200);

  json req_body;
  req_body["vector"] = {1.0f, 0.0f, 0.0f, 0.0f};
  req_body["top_k"] = 1;
  req_body["min_score"] = 0.0;
  auto res = client_->Post("/simv", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  ASSERT_GE(body["count"].get<int>(), 1);
  // A self-identical cosine score rounds to 1.0 at the shared 4-decimal policy.
  EXPECT_DOUBLE_EQ(body["results"][0]["score"].get<double>(), 1.0);
}

TEST_F(HttpServerTest, EventsModeFilterKeepsVectorlessMatches) {
  // Seed co-occurrence between "query" and four neighbors in one context. The
  // neighbors have no stored vectors, exercising the cold-start path.
  for (const char* id : {"query", "keep1", "drop1", "keep2", "drop2"}) {
    json evt;
    evt["ctx"] = "ctx1";
    evt["type"] = "ADD";
    evt["id"] = id;
    evt["score"] = 50;
    ASSERT_EQ(client_->Post("/event", evt.dump(), "application/json")->status, 200);
  }

  metadata_store_->Set("keep1", {{"status", std::string("active")}});
  metadata_store_->Set("drop1", {{"status", std::string("draft")}});
  metadata_store_->Set("keep2", {{"status", std::string("active")}});
  metadata_store_->Set("drop2", {{"status", std::string("draft")}});
  // Put more than the old fixed 3x prefix ahead of both matches.
  for (int i = 0; i < 10; ++i) {
    const std::string id = "high_drop" + std::to_string(i);
    co_index_->SetScore("query", id, 10000.0F - static_cast<float>(i));
    metadata_store_->Set(id, {{"status", std::string("draft")}});
  }

  json req_body;
  req_body["id"] = "query";
  req_body["top_k"] = 2;
  req_body["mode"] = "events";
  req_body["filter"] = "status:active";

  auto res = client_->Post("/sim", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200);

  auto body = json::parse(res->body);
  EXPECT_EQ(body["count"], 2);
  std::vector<std::string> ids;
  for (const auto& item : body["results"]) {
    ids.push_back(item["id"].get<std::string>());
  }
  EXPECT_NE(std::find(ids.begin(), ids.end(), "keep1"), ids.end());
  EXPECT_NE(std::find(ids.begin(), ids.end(), "keep2"), ids.end());
  EXPECT_EQ(std::find(ids.begin(), ids.end(), "drop1"), ids.end());
}

TEST_F(HttpServerTest, VecsetDimensionMismatchRejectedEarly) {
  json vec1;
  vec1["id"] = "first";
  vec1["vector"] = {1.0f, 0.0f, 0.0f, 0.0f};
  ASSERT_EQ(client_->Post("/vecset", vec1.dump(), "application/json")->status, 200);

  // Store dimension is now fixed at 4; a 2-dim vector must be rejected with 400.
  json vec2;
  vec2["id"] = "bad";
  vec2["vector"] = {1.0f, 0.0f};
  auto res = client_->Post("/vecset", vec2.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

TEST_F(HttpServerTest, FiniteDoubleOutsideFloatRangeIsRejectedWithoutMutation) {
  json vecset = {{"id", "huge"}, {"vector", json::array({1e300, 0.0, 0.0, 0.0})}};
  auto vecset_response = client_->Post("/vecset", vecset.dump(), "application/json");
  ASSERT_TRUE(vecset_response);
  EXPECT_EQ(vecset_response->status, 400);
  EXPECT_FALSE(vector_store_->HasVector("huge"));

  json simv = {{"vector", json::array({1e300, 0.0, 0.0, 0.0})}, {"top_k", 1}};
  auto simv_response = client_->Post("/simv", simv.dump(), "application/json");
  ASSERT_TRUE(simv_response);
  EXPECT_EQ(simv_response->status, 400);
}

TEST_F(HttpServerTest, VecsetWritesNoWalRecordWhenVectorPayloadsAreExcluded) {
  // include_vectors=false makes snapshots the vector durability boundary, so
  // no record is produced for a VECSET even when it carries metadata. A closed
  // WAL therefore cannot fail the request: if this surface still synthesised a
  // record of its own, the append would be rejected and the response would be
  // 503 instead of 200. This is the same contract the TCP path is pinned to.
  config_->wal.include_vectors = false;
  storage::WriteAheadLog closed_wal;
  handler_ctx_.wal = &closed_wal;

  json body;
  body["id"] = "snapshot-backed";
  body["vector"] = {1.0, 0.0, 0.0, 0.0};
  body["metadata"] = {{"status", "active"}};

  auto res = client_->Post("/vecset", body.dump(), "application/json");

  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200) << res->body;
  EXPECT_TRUE(vector_store_->HasVector("snapshot-backed"));
  EXPECT_FALSE(read_only_.load(std::memory_order_acquire));
  handler_ctx_.wal = nullptr;
}

TEST_F(HttpServerTest, EventScoreRangeIsEnforcedByTheSharedWritePath) {
  json body;
  body["ctx"] = "ctx1";
  body["id"] = "item1";
  body["type"] = "ADD";
  body["score"] = 101;

  auto res = client_->Post("/event", body.dump(), "application/json");

  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400) << res->body;
  EXPECT_EQ(event_store_->GetTotalEventCount(), 0U);
}

TEST_F(HttpServerTest, EventScoreBoundsAreInclusiveAndValuesOutsideThemAreRejected) {
  // Both ends of the accepted range are valid values, and the first value past
  // either end is not. Posting all four over HTTP pins the boundary rather than
  // one side of it, and the store count distinguishes a rejected write from an
  // accepted one that merely reported an error.
  const auto post_score = [this](const std::string& id, const json& score) {
    json body;
    body["ctx"] = "bounds";
    body["id"] = id;
    body["type"] = "ADD";
    body["score"] = score;
    return client_->Post("/event", body.dump(), "application/json");
  };

  auto lower_bound = post_score("at-zero", 0);
  ASSERT_TRUE(lower_bound);
  EXPECT_EQ(lower_bound->status, 200) << lower_bound->body;

  auto upper_bound = post_score("at-hundred", 100);
  ASSERT_TRUE(upper_bound);
  EXPECT_EQ(upper_bound->status, 200) << upper_bound->body;

  const uint64_t accepted = event_store_->GetTotalEventCount();
  EXPECT_EQ(accepted, 2U) << "Both boundary values should have been stored";

  auto below = post_score("below-zero", -1);
  ASSERT_TRUE(below);
  EXPECT_EQ(below->status, 400) << below->body;
  EXPECT_NE(below->body.find("[0, 100]"), std::string::npos)
      << "Rejection did not name the accepted range: " << below->body;

  auto above = post_score("above-hundred", 101);
  ASSERT_TRUE(above);
  EXPECT_EQ(above->status, 400) << above->body;
  EXPECT_NE(above->body.find("[0, 100]"), std::string::npos)
      << "Rejection did not name the accepted range: " << above->body;

  EXPECT_EQ(event_store_->GetTotalEventCount(), accepted) << "A rejected score must not reach the store";
}

TEST_F(HttpServerTest, EventScoreMustBeAnIntegerBeforeTheWriteIsAttempted) {
  // A fractional or textual score is rejected by the HTTP layer itself. The
  // shared write path cannot stand in for this check: it only ever sees an int,
  // so a fraction that got past here would be truncated to a value the store
  // happily accepts, and the request would report success for a score the
  // client never sent.
  const auto post_raw_score = [this](const std::string& id, const std::string& raw_score) {
    const std::string body = R"({"ctx":"types","id":")" + id + R"(","type":"ADD","score":)" + raw_score + "}";
    return client_->Post("/event", body, "application/json");
  };

  auto fractional = post_raw_score("fractional", "50.5");
  ASSERT_TRUE(fractional);
  EXPECT_EQ(fractional->status, 400) << fractional->body;
  EXPECT_NE(fractional->body.find("must be an integer"), std::string::npos)
      << "Fractional score was not rejected as a type error: " << fractional->body;

  auto textual = post_raw_score("textual", R"("fifty")");
  ASSERT_TRUE(textual);
  EXPECT_EQ(textual->status, 400) << textual->body;
  EXPECT_NE(textual->body.find("must be an integer"), std::string::npos)
      << "Non-numeric score was not rejected as a type error: " << textual->body;

  EXPECT_EQ(event_store_->GetTotalEventCount(), 0U) << "A non-integer score must not reach the store";
}

TEST_F(HttpServerTest, RoutesWithoutASuccessBodyAreStillCountedByType) {
  // /config and /cache/stats have no per-handler counter of their own: the
  // route declaration is what puts them in the command statistics, exactly as
  // the TCP dispatcher counts CONFIG SHOW and CACHE STATS.
  const uint64_t config_before = stats_.config_commands.load();
  const uint64_t cache_before = stats_.cache_commands.load();

  ASSERT_TRUE(client_->Get("/config"));
  ASSERT_TRUE(client_->Get("/cache/stats"));

  EXPECT_EQ(stats_.config_commands.load(), config_before + 1);
  EXPECT_EQ(stats_.cache_commands.load(), cache_before + 1);
}

TEST_F(HttpServerTest, HealthAndMetricsAreNotCountedAsCommands) {
  const uint64_t total_before = stats_.total_commands.load();

  ASSERT_TRUE(client_->Get("/health"));
  ASSERT_TRUE(client_->Get("/health/live"));
  ASSERT_TRUE(client_->Get("/health/ready"));
  ASSERT_TRUE(client_->Get("/health/detail"));
  ASSERT_TRUE(client_->Get("/metrics"));

  EXPECT_EQ(stats_.total_commands.load(), total_before);
}

TEST_F(HttpServerTest, RejectedWriteIsCountedAsAFailedCommand) {
  const uint64_t total_before = stats_.total_commands.load();
  const uint64_t failed_before = stats_.failed_commands.load();

  auto res = client_->Post("/vecset", "{not json", "application/json");

  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 400);
  EXPECT_EQ(stats_.total_commands.load(), total_before + 1);
  EXPECT_EQ(stats_.failed_commands.load(), failed_before + 1);
  EXPECT_LE(stats_.failed_commands.load(), stats_.total_commands.load());
}

TEST_F(HttpServerTest, WriteEndpointsRejectDuringLockSnapshot) {
  read_only_.store(true);
  json vec;
  vec["id"] = "blocked";
  vec["vector"] = {1.0F, 0.0F, 0.0F, 0.0F};
  auto res = client_->Post("/vecset", vec.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 503);
}

// Authentication tests: requirepass gates write/admin endpoints over HTTP
class HttpServerAuthTest : public ::testing::Test {
 protected:
  virtual void ConfigureServerConfig() {}

  void SetUp() override {
    config_ = std::make_unique<config::Config>();
    config_->vectors.default_dimension = 4;
    config_->similarity.default_top_k = 10;
    ConfigureServerConfig();

    event_store_ = std::make_unique<events::EventStore>(config_->events);
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    vector_store_ = std::make_unique<vectors::VectorStore>(config_->vectors);
    metadata_store_ = std::make_unique<vectors::MetadataStore>();
    similarity_engine_ =
        std::make_unique<similarity::SimilarityEngine>(event_store_.get(), co_index_.get(), vector_store_.get(),
                                                       config_->similarity, config_->vectors, metadata_store_.get());
    cache_ = std::make_unique<cache::SimilarityCache>(10 * 1024 * 1024, 0.1);

    handler_ctx_.event_store = event_store_.get();
    handler_ctx_.co_index = co_index_.get();
    handler_ctx_.vector_store = vector_store_.get();
    handler_ctx_.metadata_store = metadata_store_.get();
    handler_ctx_.similarity_engine = similarity_engine_.get();
    handler_ctx_.cache = cache_.get();
    handler_ctx_.config = config_.get();
    handler_ctx_.requirepass = kPassword;

    server::HttpServerConfig http_config;
    http_config.bind = "127.0.0.1";
    http_config.port = 0;
    http_config.allow_cidrs = {"127.0.0.0/8"};
    http_config.requirepass = kPassword;

    http_server_ = std::make_unique<server::HttpServer>(http_config, &handler_ctx_, config_.get(), &loading_, &stats_);
    ASSERT_TRUE(http_server_->Start());
    const int port = http_server_->GetPort();
    ASSERT_GT(port, 0) << "HTTP server did not report a bound port";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    client_ = std::make_unique<httplib::Client>("http://127.0.0.1:" + std::to_string(port));
  }

  void TearDown() override {
    client_.reset();
    if (http_server_) {
      http_server_->Stop();
    }
  }

  static constexpr const char* kPassword = "s3cret";

  std::unique_ptr<config::Config> config_;
  std::unique_ptr<events::EventStore> event_store_;
  std::unique_ptr<events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<vectors::VectorStore> vector_store_;
  std::unique_ptr<vectors::MetadataStore> metadata_store_;
  std::unique_ptr<similarity::SimilarityEngine> similarity_engine_;
  std::unique_ptr<cache::SimilarityCache> cache_;

  server::ServerStats stats_;
  std::atomic<bool> loading_{false};
  std::atomic<bool> read_only_{false};
  server::HandlerContext handler_ctx_{
      .event_store = nullptr,
      .co_index = nullptr,
      .vector_store = nullptr,
      .metadata_store = nullptr,
      .similarity_engine = nullptr,
      .cache = nullptr,
      .stats = stats_,
      .config = nullptr,
      .loading = loading_,
      .read_only = read_only_,
      .dump_dir = "",
  };

  std::unique_ptr<server::HttpServer> http_server_;
  std::unique_ptr<httplib::Client> client_;
};

TEST_F(HttpServerAuthTest, UnauthenticatedWriteRejected) {
  json vec;
  vec["id"] = "x";
  vec["vector"] = {0.1f, 0.2f, 0.3f, 0.4f};
  auto res = client_->Post("/vecset", vec.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(HttpServerAuthTest, WrongPasswordRejected) {
  httplib::Headers headers = {{"Authorization", "Bearer wrong"}};
  json vec;
  vec["id"] = "x";
  vec["vector"] = {0.1f, 0.2f, 0.3f, 0.4f};
  auto res = client_->Post("/vecset", headers, vec.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(HttpServerAuthTest, BearerTokenAllowsWrite) {
  httplib::Headers headers = {{"Authorization", std::string("Bearer ") + kPassword}};
  json vec;
  vec["id"] = "x";
  vec["vector"] = {0.1f, 0.2f, 0.3f, 0.4f};
  auto res = client_->Post("/vecset", headers, vec.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

TEST_F(HttpServerAuthTest, BasicAuthAllowsWrite) {
  // base64("user:s3cret") = dXNlcjpzM2NyZXQ=
  httplib::Headers headers = {{"Authorization", "Basic dXNlcjpzM2NyZXQ="}};
  json vec;
  vec["id"] = "y";
  vec["vector"] = {0.1f, 0.2f, 0.3f, 0.4f};
  auto res = client_->Post("/vecset", headers, vec.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

TEST_F(HttpServerAuthTest, ReadEndpointsStayOpen) {
  // Read-only endpoints are not gated even when a password is configured.
  auto res = client_->Get("/info");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  auto health = client_->Get("/health/live");
  ASSERT_TRUE(health);
  EXPECT_EQ(health->status, 200);
}

TEST_F(HttpServerAuthTest, UnauthenticatedDumpSaveRejected) {
  json req;
  req["filepath"] = "test.dmp";
  auto res = client_->Post("/dump/save", req.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(HttpServerAuthTest, UnauthenticatedDumpStatusRejected) {
  auto res = client_->Get("/dump/status");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

namespace {

/// One row of the HTTP gating contract.
struct GatedRoute {
  const char* method;
  const char* path;
  const char* body;
};

/// Every route whose command type carries a write or admin privilege. The
/// route table derives the gate from GetCommandPrivilege, so this list is the
/// externally observable form of that classification.
const std::array<GatedRoute, 12> kGatedRoutes = {
    GatedRoute{"POST", "/event", R"({"ctx":"c","id":"gated","type":"ADD","score":1})"},
    GatedRoute{"POST", "/vecset", R"({"id":"gated","vector":[0.1,0.2,0.3,0.4]})"},
    GatedRoute{"DELETE", "/vecset", R"({"id":"seeded"})"},
    GatedRoute{"POST", "/metaset", R"({"id":"seeded","metadata":{"k":"v"}})"},
    GatedRoute{"POST", "/dump/save", R"({"filepath":"gated.dmp"})"},
    GatedRoute{"POST", "/dump/load", R"({"filepath":"gated.dmp"})"},
    GatedRoute{"POST", "/dump/verify", R"({"filepath":"gated.dmp"})"},
    GatedRoute{"POST", "/dump/info", R"({"filepath":"gated.dmp"})"},
    GatedRoute{"GET", "/dump/status", ""},
    GatedRoute{"POST", "/cache/clear", R"({"scope":"all"})"},
    GatedRoute{"POST", "/cache/enable", ""},
    GatedRoute{"POST", "/cache/disable", ""}};

}  // namespace

TEST_F(HttpServerAuthTest, EveryGatedRouteRejectsAnAbsentCredential) {
  ASSERT_TRUE(vector_store_->SetVector("seeded", {0.1F, 0.2F, 0.3F, 0.4F}).has_value());
  metadata_store_->Set("seeded", {{"k", std::string("original")}});

  for (const auto& route : kGatedRoutes) {
    httplib::Result res =
        (std::string(route.method) == "GET")
            ? client_->Get(route.path)
            : (std::string(route.method) == "DELETE" ? client_->Delete(route.path, route.body, "application/json")
                                                     : client_->Post(route.path, route.body, "application/json"));
    ASSERT_TRUE(res) << route.method << " " << route.path;
    EXPECT_EQ(res->status, 401) << route.method << " " << route.path;
  }

  // No gated request may have changed state on its way to the rejection.
  EXPECT_TRUE(vector_store_->HasVector("seeded"));
  EXPECT_FALSE(vector_store_->HasVector("gated"));
  ASSERT_NE(metadata_store_->Get("seeded"), nullptr);
  EXPECT_EQ(std::get<std::string>(metadata_store_->Get("seeded")->at("k")), "original");
  EXPECT_EQ(event_store_->GetTotalEventCount(), 0U);
}

TEST_F(HttpServerAuthTest, EveryGatedRouteAcceptsTheConfiguredCredential) {
  // The same routes must stop returning 401 once the credential is presented;
  // otherwise the gate would be indistinguishable from a route that is closed
  // outright.
  const httplib::Headers headers = {{"Authorization", std::string("Bearer ") + kPassword}};
  for (const auto& route : kGatedRoutes) {
    httplib::Result res = (std::string(route.method) == "GET")
                              ? client_->Get(route.path, headers)
                              : (std::string(route.method) == "DELETE"
                                     ? client_->Delete(route.path, headers, route.body, "application/json")
                                     : client_->Post(route.path, headers, route.body, "application/json"));
    ASSERT_TRUE(res) << route.method << " " << route.path;
    EXPECT_NE(res->status, 401) << route.method << " " << route.path;
  }
}

TEST_F(HttpServerAuthTest, DocumentedPublicRoutesAreNeverGated) {
  const std::array<const char*, 7> get_routes = {"/health", "/health/live", "/health/ready", "/health/detail",
                                                 "/info",   "/config",      "/metrics"};
  for (const auto* path : get_routes) {
    auto res = client_->Get(path);
    ASSERT_TRUE(res) << path;
    EXPECT_NE(res->status, 401) << path;
  }

  auto cache_stats = client_->Get("/cache/stats");
  ASSERT_TRUE(cache_stats);
  EXPECT_NE(cache_stats->status, 401);

  auto sim = client_->Post("/sim", R"({"id":"anything"})", "application/json");
  ASSERT_TRUE(sim);
  EXPECT_NE(sim->status, 401);

  auto simv = client_->Post("/simv", R"({"vector":[0.1,0.2,0.3,0.4]})", "application/json");
  ASSERT_TRUE(simv);
  EXPECT_NE(simv->status, 401);
}

TEST_F(HttpServerAuthTest, NonConformingAuthorizationValuesAreRejected) {
  // "Bearer " is absent deliberately. Trailing whitespace is optional
  // whitespace around a field value, stripped while the header is parsed, so by
  // the time the server compares anything it holds "Bearer" -- the entry above.
  // It reads like a separate case and is not one.
  const std::array<const char*, 6> rejected = {
      "",                       // present but empty
      "Bearer",                 // scheme without a credential
      "Bearer wrong",           // wrong credential
      "Token s3cret",           // unknown scheme
      "Basic !!not-base64!!",   // undecodable Basic payload
      "Basic dXNlcjp3cm9uZw=="  // base64("user:wrong")
  };
  const json body = {{"id", "rejected"}, {"vector", {0.1F, 0.2F, 0.3F, 0.4F}}};
  for (const auto* value : rejected) {
    httplib::Headers headers = {{"Authorization", value}};
    auto res = client_->Post("/vecset", headers, body.dump(), "application/json");
    // Bracketed so a value that is empty or ends in a space is legible here.
    ASSERT_TRUE(res) << "Authorization: [" << value << "]";
    EXPECT_EQ(res->status, 401) << "Authorization: [" << value << "]";
  }
  EXPECT_FALSE(vector_store_->HasVector("rejected"));
}

TEST_F(HttpServerAuthTest, BasicPayloadWithoutColonIsTreatedAsThePasswordItself) {
  // Deliberate current behavior: the username is ignored, and a payload with
  // no colon is the password. Pinned so it is changed on purpose, not by
  // accident.
  const json body = {{"id", "colonless"}, {"vector", {0.1F, 0.2F, 0.3F, 0.4F}}};
  httplib::Headers headers = {{"Authorization", "Basic czNjcmV0"}};  // base64("s3cret")
  auto res = client_->Post("/vecset", headers, body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

TEST_F(HttpServerAuthTest, RejectedRequestIsAccountedAsExactlyOneFailedCommand) {
  const uint64_t total_before = stats_.total_commands.load();
  const uint64_t failed_before = stats_.failed_commands.load();

  const json body = {{"id", "unauthorized"}, {"vector", {0.1F, 0.2F, 0.3F, 0.4F}}};
  auto res = client_->Post("/vecset", body.dump(), "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 401);

  EXPECT_EQ(stats_.total_commands.load(), total_before + 1);
  EXPECT_EQ(stats_.failed_commands.load(), failed_before + 1);
  EXPECT_LE(stats_.failed_commands.load(), stats_.total_commands.load());
}

TEST_F(HttpServerAuthTest, AcceptedRequestIsAccountedWithoutAFailure) {
  const httplib::Headers headers = {{"Authorization", std::string("Bearer ") + kPassword}};
  const uint64_t total_before = stats_.total_commands.load();
  const uint64_t failed_before = stats_.failed_commands.load();
  const uint64_t vecset_before = stats_.vecset_commands.load();

  const json body = {{"id", "authorized"}, {"vector", {0.1F, 0.2F, 0.3F, 0.4F}}};
  auto res = client_->Post("/vecset", headers, body.dump(), "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200);

  EXPECT_EQ(stats_.total_commands.load(), total_before + 1);
  EXPECT_EQ(stats_.failed_commands.load(), failed_before);
  EXPECT_EQ(stats_.vecset_commands.load(), vecset_before + 1);
}

class HttpServerRateLimitTest : public HttpServerAuthTest {
 protected:
  // Smallest bucket the configuration schema accepts, so the burst below
  // outruns the refill by the widest margin the product allows.
  static constexpr int kCapacity = 1;
  static constexpr int kRefillPerSec = 1;

  void ConfigureServerConfig() override {
    config_->api.rate_limiting.enable = true;
    config_->api.rate_limiting.capacity = kCapacity;
    config_->api.rate_limiting.refill_rate = kRefillPerSec;
    config_->api.rate_limiting.max_clients = 10;
  }
};

TEST_F(HttpServerRateLimitTest, AppliesConfiguredLimitToHttpRequests) {
  // The token bucket refills against a steady clock, so the number of admitted
  // requests is only bounded by what elapsed while the burst ran. Measuring
  // that elapsed time and asserting the bucket invariant against it keeps the
  // expectation exact under any scheduling delay, where asserting that two
  // consecutive requests land inside the refill interval would not.
  constexpr int kBurst = 50;
  int allowed = 0;
  int limited = 0;

  const auto started = std::chrono::steady_clock::now();
  for (int i = 0; i < kBurst; ++i) {
    auto res = client_->Get("/health/live");
    ASSERT_TRUE(res) << "Request " << i << " produced no response";
    if (res->status == 200) {
      ++allowed;
    } else {
      EXPECT_EQ(res->status, 429) << "Unexpected status for request " << i;
      ++limited;
    }
  }
  const double elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  EXPECT_EQ(allowed + limited, kBurst);
  EXPECT_GE(allowed, 1);

  // A bucket that starts full can admit its capacity plus whatever the refill
  // granted over the measured window, and nothing more. The window is measured
  // from before the first request to after the last, so it can only overstate
  // what the limiter had available.
  const double max_admissible = kCapacity + (elapsed_sec * kRefillPerSec);
  EXPECT_LE(allowed, max_admissible) << "Admitted " << allowed << " requests in " << elapsed_sec
                                     << "s, more than the configured bucket allows";

  // Conversely, the burst can only be served in full if the refill covered it.
  // Asserting the rejection only when the measurement rules that out keeps the
  // check honest on a machine slow enough to have earned the tokens.
  if (elapsed_sec * kRefillPerSec < kBurst - kCapacity) {
    EXPECT_GT(limited, 0) << "No request was rejected in " << elapsed_sec << "s despite the configured limit";
  }
}

class HttpServerAdmissionTest : public HttpServerTest {
 protected:
  void ConfigureHttpServer(server::HttpServerConfig* config) override {
    config->worker_threads = 2;
    config->max_queued_connections = 2;
    config->max_connections = 2;
    config->max_connections_per_ip = 1;
    config->read_timeout_sec = 2;
  }
};

TEST_F(HttpServerAdmissionTest, RejectsSecondSlowConnectionFromSameIpBeforeParsing) {
  auto connect_socket = [port = static_cast<uint16_t>(port_)]() {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
      return socket_fd;
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      ::close(socket_fd);
      return -1;
    }
    return socket_fd;
  };

  const int first = connect_socket();
  ASSERT_GE(first, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const int second = connect_socket();
  ASSERT_GE(second, 0);

  timeval timeout = {};
  timeout.tv_sec = 1;
  ASSERT_EQ(::setsockopt(second, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
  char byte = 0;
  EXPECT_LE(::recv(second, &byte, 1, 0), 0);

  ::close(second);
  ::close(first);
}

// CORS behavior: when enabled but no origin is configured, the
// Access-Control-Allow-Origin header must be omitted (not emitted as "null").
class HttpServerCorsTest : public ::testing::Test {
 protected:
  void StartServer(const std::string& origin) {
    config_ = std::make_unique<config::Config>();
    config_->vectors.default_dimension = 4;

    event_store_ = std::make_unique<events::EventStore>(config_->events);
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    vector_store_ = std::make_unique<vectors::VectorStore>(config_->vectors);
    metadata_store_ = std::make_unique<vectors::MetadataStore>();
    similarity_engine_ =
        std::make_unique<similarity::SimilarityEngine>(event_store_.get(), co_index_.get(), vector_store_.get(),
                                                       config_->similarity, config_->vectors, metadata_store_.get());

    handler_ctx_.event_store = event_store_.get();
    handler_ctx_.co_index = co_index_.get();
    handler_ctx_.vector_store = vector_store_.get();
    handler_ctx_.metadata_store = metadata_store_.get();
    handler_ctx_.similarity_engine = similarity_engine_.get();
    handler_ctx_.config = config_.get();

    server::HttpServerConfig http_config;
    http_config.bind = "127.0.0.1";
    http_config.port = 0;
    http_config.allow_cidrs = {"127.0.0.0/8"};
    http_config.enable_cors = true;
    http_config.cors_allow_origin = origin;

    http_server_ = std::make_unique<server::HttpServer>(http_config, &handler_ctx_, config_.get(), &loading_, &stats_);
    ASSERT_TRUE(http_server_->Start());
    const int port = http_server_->GetPort();
    ASSERT_GT(port, 0) << "HTTP server did not report a bound port";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    client_ = std::make_unique<httplib::Client>("http://127.0.0.1:" + std::to_string(port));
  }

  void TearDown() override {
    client_.reset();
    if (http_server_) {
      http_server_->Stop();
    }
  }

  std::unique_ptr<config::Config> config_;
  std::unique_ptr<events::EventStore> event_store_;
  std::unique_ptr<events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<vectors::VectorStore> vector_store_;
  std::unique_ptr<vectors::MetadataStore> metadata_store_;
  std::unique_ptr<similarity::SimilarityEngine> similarity_engine_;

  server::ServerStats stats_;
  std::atomic<bool> loading_{false};
  std::atomic<bool> read_only_{false};
  server::HandlerContext handler_ctx_{
      .event_store = nullptr,
      .co_index = nullptr,
      .vector_store = nullptr,
      .metadata_store = nullptr,
      .similarity_engine = nullptr,
      .cache = nullptr,
      .stats = stats_,
      .config = nullptr,
      .loading = loading_,
      .read_only = read_only_,
      .dump_dir = "",
  };

  std::unique_ptr<server::HttpServer> http_server_;
  std::unique_ptr<httplib::Client> client_;
};

TEST_F(HttpServerCorsTest, EmptyOriginOmitsAcaoHeader) {
  StartServer("");
  auto res = client_->Get("/health/live");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_FALSE(res->has_header("Access-Control-Allow-Origin"));
}

TEST_F(HttpServerCorsTest, ConfiguredOriginEmitsAcaoHeader) {
  StartServer("https://example.com");
  auto res = client_->Get("/health/live");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  ASSERT_TRUE(res->has_header("Access-Control-Allow-Origin"));
  EXPECT_EQ(res->get_header_value("Access-Control-Allow-Origin"), "https://example.com");
}

TEST_F(HttpServerCorsTest, PreflightAllowsAuthenticatedDeleteRoutes) {
  StartServer("https://example.com");
  httplib::Headers headers = {{"Origin", "https://example.com"},
                              {"Access-Control-Request-Method", "DELETE"},
                              {"Access-Control-Request-Headers", "Authorization, Content-Type"}};
  auto res = client_->Options("/vecset", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 204);
  EXPECT_NE(res->get_header_value("Access-Control-Allow-Methods").find("DELETE"), std::string::npos);
  EXPECT_NE(res->get_header_value("Access-Control-Allow-Headers").find("Authorization"), std::string::npos);
}

}  // namespace

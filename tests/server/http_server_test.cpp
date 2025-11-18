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

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

#include "cache/similarity_cache.h"
#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/http_server.h"
#include "server/server_types.h"
#include "similarity/similarity_engine.h"
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

    // Initialize components
    event_store_ = std::make_unique<events::EventStore>(config_->events);
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    vector_store_ = std::make_unique<vectors::VectorStore>(config_->vectors);
    similarity_engine_ = std::make_unique<similarity::SimilarityEngine>(
        event_store_.get(), co_index_.get(), vector_store_.get(), config_->similarity);
    cache_ = std::make_unique<cache::SimilarityCache>(10 * 1024 * 1024, 0.1);

    // Setup handler context (pointers only, references already set in initializer)
    handler_ctx_.event_store = event_store_.get();
    handler_ctx_.co_index = co_index_.get();
    handler_ctx_.vector_store = vector_store_.get();
    handler_ctx_.similarity_engine = similarity_engine_.get();
    handler_ctx_.cache = cache_.get();
    handler_ctx_.config = config_.get();

    // Create HTTP server
    server::HttpServerConfig http_config;
    http_config.bind = "127.0.0.1";
    http_config.port = 18081;  // Use different port for testing
    http_config.allow_cidrs = {"127.0.0.0/8"};

    http_server_ = std::make_unique<server::HttpServer>(http_config, &handler_ctx_, config_.get(), &loading_, &stats_);

    // Start server
    auto result = http_server_->Start();
    ASSERT_TRUE(result) << "Failed to start HTTP server: " << result.error().message();

    // Wait for server to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Create HTTP client
    client_ = std::make_unique<httplib::Client>("http://127.0.0.1:18081");
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
  std::unique_ptr<similarity::SimilarityEngine> similarity_engine_;
  std::unique_ptr<cache::SimilarityCache> cache_;

  server::ServerStats stats_;
  std::atomic<bool> loading_{false};
  std::atomic<bool> read_only_{false};
  server::HandlerContext handler_ctx_{
      .event_store = nullptr,
      .co_index = nullptr,
      .vector_store = nullptr,
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

TEST_F(HttpServerTest, SimNotFound) {
  json req_body;
  req_body["id"] = "nonexistent";
  req_body["top_k"] = 5;
  req_body["mode"] = "vectors";

  auto res = client_->Post("/sim", req_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
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

}  // namespace

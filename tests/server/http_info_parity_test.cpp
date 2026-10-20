/**
 * @file http_info_parity_test.cpp
 * @brief HTTP /info, /metrics and /dump/status report the same figures as their TCP counterparts
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/handlers/dump_handler.h"
#include "server/handlers/info_handler.h"
#include "server/http_server.h"
#include "server/server_types.h"
#include "storage/snapshot_fork.h"
#include "vectors/vector_store.h"

using json = nlohmann::json;
using namespace nvecd;

namespace {

class HttpInfoParityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_ = std::make_unique<config::Config>();
    config_->vectors.default_dimension = 4;
    event_store_ = std::make_unique<events::EventStore>(config_->events);
    co_index_ = std::make_unique<events::CoOccurrenceIndex>();
    vector_store_ = std::make_unique<vectors::VectorStore>(config_->vectors);
    handler_ctx_.event_store = event_store_.get();
    handler_ctx_.co_index = co_index_.get();
    handler_ctx_.vector_store = vector_store_.get();
    handler_ctx_.config = config_.get();
    handler_ctx_.write_serialization_gate = &write_serialization_gate_;

    server::HttpServerConfig http_config;
    http_config.bind = "127.0.0.1";
    http_config.port = 0;
    http_config.allow_cidrs = {"127.0.0.0/8"};
    http_server_ = std::make_unique<server::HttpServer>(http_config, &handler_ctx_, config_.get(), &loading_, &stats_);
    auto started = http_server_->Start();
    ASSERT_TRUE(started) << started.error().message();
    client_ = std::make_unique<httplib::Client>("http://127.0.0.1:" + std::to_string(http_server_->GetPort()));
  }

  void TearDown() override {
    client_.reset();
    if (http_server_) {
      http_server_->Stop();
    }
    fork_writer_.WaitForChild();
    handler_ctx_.fork_snapshot_writer = nullptr;
    if (!snapshot_dir_.empty()) {
      std::filesystem::remove_all(snapshot_dir_);
    }
  }

  /// Attach a fork writer and a scratch directory for snapshot files.
  void AttachForkWriter() {
    snapshot_dir_ = std::filesystem::temp_directory_path() / ("nvecd_http_parity_" + std::to_string(::getpid()));
    std::filesystem::remove_all(snapshot_dir_);
    std::filesystem::create_directories(snapshot_dir_);
    handler_ctx_.fork_snapshot_writer = &fork_writer_;
  }

  /// "key: value" fields of the TCP DUMP STATUS block, without the status line and END.
  static std::map<std::string, std::string> TcpStatusFields(const std::string& block) {
    std::map<std::string, std::string> fields;
    std::istringstream lines(block);
    std::string line;
    while (std::getline(lines, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      const auto sep = line.find(": ");
      if (sep != std::string::npos) {
        fields[line.substr(0, sep)] = line.substr(sep + 2);
      }
    }
    return fields;
  }

  /// Read DUMP STATUS over both surfaces and require the same keys and values.
  /// Returns the shared field map for state-specific assertions.
  std::map<std::string, std::string> ExpectDumpStatusParity() {
    auto tcp = server::handlers::HandleDumpStatus(handler_ctx_);
    EXPECT_TRUE(tcp.has_value());
    auto http = client_->Get("/dump/status");
    EXPECT_TRUE(http);
    if (!tcp || !http) {
      return {};
    }
    EXPECT_EQ(http->status, 200);
    const auto body = json::parse(http->body);
    EXPECT_EQ(body["status"], "ok");
    EXPECT_FALSE(body.contains("data"));
    const auto fields = TcpStatusFields(*tcp);
    EXPECT_EQ(body["snapshot"], json(fields));
    return fields;
  }

  /// Value of "<key>: " in a TCP INFO block.
  static std::string InfoField(const std::string& info, const std::string& key) {
    std::istringstream lines(info);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.rfind(key + ": ", 0) == 0) {
        return line.substr(key.size() + 2);
      }
    }
    return {};
  }

  std::unique_ptr<config::Config> config_;
  std::unique_ptr<events::EventStore> event_store_;
  std::unique_ptr<events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<vectors::VectorStore> vector_store_;
  server::ServerStats stats_;
  std::atomic<bool> loading_{false};
  std::atomic<bool> read_only_{false};
  std::mutex write_serialization_gate_;
  server::HandlerContext handler_ctx_{.stats = stats_, .loading = loading_, .read_only = read_only_};
  storage::ForkSnapshotWriter fork_writer_;
  std::filesystem::path snapshot_dir_;
  std::unique_ptr<server::HttpServer> http_server_;
  std::unique_ptr<httplib::Client> client_;
};

TEST_F(HttpInfoParityTest, UsedMemoryBytesMatchesTcpInfo) {
  ASSERT_TRUE(vector_store_->SetVector("a", {1.0F, 0.0F, 0.0F, 0.0F}).has_value());
  ASSERT_TRUE(vector_store_->SetVector("b", {0.0F, 1.0F, 0.0F, 0.0F}).has_value());

  auto tcp = server::handlers::HandleInfo(handler_ctx_);
  ASSERT_TRUE(tcp.has_value());
  auto http = client_->Get("/info");
  ASSERT_TRUE(http);
  ASSERT_EQ(http->status, 200);

  const auto body = json::parse(http->body);
  EXPECT_EQ(std::to_string(body["memory"]["used_memory_bytes"].get<uint64_t>()), InfoField(*tcp, "used_memory_bytes"));
  EXPECT_GE(body["memory"]["used_memory_stores_bytes"].get<uint64_t>(),
            body["memory"]["used_memory_bytes"].get<uint64_t>());
}

TEST_F(HttpInfoParityTest, InfoReportsWalReplayGapAndCommandStats) {
  stats_.wal_replay_records_skipped.store(3);
  json event = {{"ctx", "c"}, {"type", "ADD"}, {"id", "x"}, {"score", 1}};
  ASSERT_EQ(client_->Post("/event", event.dump(), "application/json")->status, 200);
  ASSERT_EQ(client_->Post("/vecset", "{bad", "application/json")->status, 400);

  auto http = client_->Get("/info");
  ASSERT_TRUE(http);
  const auto body = json::parse(http->body);
  EXPECT_EQ(body["wal_replay_records_skipped"], 3);
  EXPECT_EQ(body["commandstats"]["event"]["calls"], 1);
  EXPECT_EQ(body["commandstats"]["event"]["failed"], 0);
  EXPECT_EQ(body["commandstats"]["vecset"]["calls"], 1);
  EXPECT_EQ(body["commandstats"]["vecset"]["failed"], 1);

  auto tcp = server::handlers::HandleInfo(handler_ctx_);
  ASSERT_TRUE(tcp.has_value());
  EXPECT_EQ(InfoField(*tcp, "cmdstat_vecset"), "calls=1,failed=1");
  EXPECT_EQ(InfoField(*tcp, "wal_replay_records_skipped"), "3");
}

TEST_F(HttpInfoParityTest, MetricsHaveOnlyLabelledCommandSeriesThatSumToTheTotal) {
  json event = {{"ctx", "c"}, {"type", "ADD"}, {"id", "x"}, {"score", 1}};
  ASSERT_EQ(client_->Post("/event", event.dump(), "application/json")->status, 200);
  ASSERT_EQ(client_->Post("/vecset", "{bad", "application/json")->status, 400);

  auto res = client_->Get("/metrics");
  ASSERT_TRUE(res);

  uint64_t calls = 0;
  uint64_t failed = 0;
  std::istringstream lines(res->body);
  std::string line;
  while (std::getline(lines, line)) {
    EXPECT_NE(line.rfind("nvecd_commands_total ", 0), 0U) << "unlabelled series double counts: " << line;
    EXPECT_NE(line.rfind("nvecd_commands_failed_total ", 0), 0U) << line;
    if (line.rfind("nvecd_commands_total{command=\"", 0) == 0) {
      calls += std::stoull(line.substr(line.rfind(' ') + 1));
    } else if (line.rfind("nvecd_commands_failed_total{command=\"", 0) == 0) {
      failed += std::stoull(line.substr(line.rfind(' ') + 1));
    }
  }
  EXPECT_EQ(calls, stats_.total_commands.load());
  EXPECT_EQ(failed, stats_.failed_commands.load());
  EXPECT_EQ(failed, 1U);
  EXPECT_NE(res->body.find("nvecd_commands_failed_total{command=\"vecset\"} 1"), std::string::npos);
}

TEST_F(HttpInfoParityTest, DumpStatusIdleUsesTheTcpFieldsAndSpelling) {
  auto tcp = server::handlers::HandleDumpStatus(handler_ctx_);
  ASSERT_TRUE(tcp.has_value());
  EXPECT_NE(tcp->find("status: idle"), std::string::npos);

  const auto fields = ExpectDumpStatusParity();
  EXPECT_EQ(fields, (std::map<std::string, std::string>{{"status", "idle"}}));

  // A writer that has not run reports the same block.
  AttachForkWriter();
  EXPECT_EQ(ExpectDumpStatusParity(), fields);
}

TEST_F(HttpInfoParityTest, DumpStatusCompletedUsesTheTcpFieldsAndSpelling) {
  AttachForkWriter();
  const std::string path = (snapshot_dir_ / "parity.nvec").string();
  ASSERT_TRUE(server::handlers::StartForkSnapshot(handler_ctx_, path).has_value());
  fork_writer_.WaitForChild(10000);

  const auto fields = ExpectDumpStatusParity();
  EXPECT_EQ(fields.at("status"), "completed");
  EXPECT_EQ(fields.at("filepath"), path);
  std::set<std::string> keys;
  for (const auto& [key, value] : fields) {
    keys.insert(key);
  }
  EXPECT_EQ(keys, (std::set<std::string>{"status", "filepath", "start_time", "end_time"}));
}

TEST_F(HttpInfoParityTest, DumpStatusFailedUsesTheTcpFieldsAndSpelling) {
  AttachForkWriter();
  const std::string path = (snapshot_dir_ / "missing_dir" / "parity.nvec").string();
  ASSERT_TRUE(server::handlers::StartForkSnapshot(handler_ctx_, path).has_value());
  fork_writer_.WaitForChild(10000);

  const auto fields = ExpectDumpStatusParity();
  EXPECT_EQ(fields.at("status"), "failed");
  EXPECT_EQ(fields.at("filepath"), path);
  EXPECT_FALSE(fields.at("error").empty());
  std::set<std::string> keys;
  for (const auto& [key, value] : fields) {
    keys.insert(key);
  }
  EXPECT_EQ(keys, (std::set<std::string>{"status", "filepath", "start_time", "end_time", "error"}));
}

}  // namespace

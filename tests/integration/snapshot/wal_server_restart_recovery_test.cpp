/**
 * @file wal_server_restart_recovery_test.cpp
 * @brief End-to-end restart recovery through the real NvecdServer startup path
 *
 * Exercises the production crash-recovery flow: a brand-new NvecdServer started
 * against an existing snapshot + WAL directory must auto-load the latest
 * checkpointed snapshot and replay only the WAL tail beyond its checkpoint. This
 * proves that pre-snapshot state survives WAL truncation and that post-snapshot
 * ops are recovered without double-counting co-occurrence scores.
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/nvecd_server.h"
#include "storage/snapshot_format_v1.h"
#include "vectors/vector_store.h"

namespace fs = std::filesystem;

using namespace nvecd;
using namespace nvecd::server;

namespace {

/// Minimal blocking TCP client for the nvecd text protocol.
class TcpClient {
 public:
  TcpClient(const std::string& host, uint16_t port) {
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
      throw std::runtime_error("Failed to create socket");
    }
    struct sockaddr_in server_addr {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr);
    if (connect(sock_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
      close(sock_);
      throw std::runtime_error("Failed to connect");
    }
  }

  ~TcpClient() { Close(); }

  void Close() {
    if (sock_ >= 0) {
      close(sock_);
      sock_ = -1;
    }
  }

  std::string SendCommand(const std::string& command) {
    std::string request = command + "\r\n";
    send(sock_, request.c_str(), request.length(), 0);
    char buffer[65536];
    ssize_t received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
      return "";
    }
    buffer[received] = '\0';
    return std::string(buffer);
  }

 private:
  int sock_ = -1;
};

/**
 * @brief Parse the score of @p neighbor_id from a "SIM ... using=events" reply.
 *
 * The reply is "OK RESULTS N\r\n<id> <score>\r\n...". Returns -1.0 if the
 * neighbor is absent.
 */
float ParseEventScore(const std::string& reply, const std::string& neighbor_id) {
  size_t pos = 0;
  while ((pos = reply.find('\n', pos)) != std::string::npos) {
    ++pos;
    size_t line_end = reply.find('\n', pos);
    std::string line = reply.substr(pos, line_end == std::string::npos ? std::string::npos : line_end - pos);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    size_t sep = line.find(' ');
    if (sep != std::string::npos && line.substr(0, sep) == neighbor_id) {
      try {
        return std::stof(line.substr(sep + 1));
      } catch (...) {
        return -1.0F;
      }
    }
  }
  return -1.0F;
}

config::Config MakeConfig(const std::string& snapshot_dir, const std::string& wal_dir) {
  config::Config config;
  config.api.tcp.bind = "127.0.0.1";
  config.api.tcp.port = 0;  // random port
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.perf.max_connections = 10;
  config.perf.thread_pool_size = 4;

  config.snapshot.dir = snapshot_dir;
  config.snapshot.mode = "lock";     // synchronous save => deterministic checkpoint+truncate
  config.snapshot.interval_sec = 0;  // no auto-snapshot scheduler

  config.wal.enabled = true;
  config.wal.dir = wal_dir;
  config.wal.sync_on_write = true;

  config.events.ctx_buffer_size = 100;
  config.events.decay_interval_sec = 300;

  config.similarity.default_top_k = 10;
  config.similarity.max_top_k = 100;
  return config;
}

}  // namespace

// ============================================================================
// Real-server restart recovery: snapshot auto-load + WAL replay, no double-count.
// ============================================================================

TEST(WalServerRestartRecovery, SnapshotPlusWalTailRecoveredOnRestart) {
  const auto root =
      fs::temp_directory_path() / ("nvecd_wal_restart_recovery_" + std::to_string(static_cast<unsigned>(::getpid())));
  fs::remove_all(root);
  const std::string snapshot_dir = (root / "snapshots").string();
  const std::string wal_dir = (root / "wal").string();
  fs::create_directories(snapshot_dir);
  fs::create_directories(wal_dir);

  float pre_snapshot_score = -1.0F;
  float post_snapshot_score = -1.0F;

  // --- First server lifetime: ingest, snapshot (checkpoint+truncate), ingest. ---
  {
    config::Config config = MakeConfig(snapshot_dir, wal_dir);
    NvecdServer server(config);
    ASSERT_TRUE(server.Start().has_value());
    const uint16_t port = server.GetPort();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
      TcpClient client("127.0.0.1", port);

      // Pre-snapshot co-occurrence between item_a and item_b in ctx1.
      ASSERT_EQ(client.SendCommand("EVENT ctx1 ADD item_a 90").find("OK"), 0U);
      ASSERT_EQ(client.SendCommand("EVENT ctx1 ADD item_b 80").find("OK"), 0U);
      ASSERT_EQ(client.SendCommand("VECSET item_a 1 0").find("OK"), 0U);
      ASSERT_EQ(client.SendCommand("VECSET item_b 0 1").find("OK"), 0U);
      ASSERT_EQ(client.SendCommand("METASET item_a status:active").find("OK"), 0U);

      pre_snapshot_score = ParseEventScore(client.SendCommand("SIM item_a 10 using=events"), "item_b");
      ASSERT_GT(pre_snapshot_score, 0.0F);

      // The last record at or below the checkpoint is not idempotent: replaying
      // it again would find nothing to delete and count a skipped record.
      ASSERT_EQ(client.SendCommand("VECSET item_d 1 1").find("OK"), 0U);
      ASSERT_EQ(client.SendCommand("VECDEL item_d").find("OK"), 0U);

      // Lock-mode DUMP SAVE writes the checkpoint sidecar and truncates the WAL.
      ASSERT_NE(client.SendCommand("DUMP SAVE").find("OK"), std::string::npos);

      // Post-snapshot ops: a new co-occurring item and a new vector.
      ASSERT_EQ(client.SendCommand("EVENT ctx2 ADD item_a 70").find("OK"), 0U);
      ASSERT_EQ(client.SendCommand("EVENT ctx2 ADD item_c 60").find("OK"), 0U);
      ASSERT_EQ(client.SendCommand("VECSET item_c 0 1").find("OK"), 0U);
      post_snapshot_score = ParseEventScore(client.SendCommand("SIM item_a 10 using=events"), "item_c");
      ASSERT_GT(post_snapshot_score, 0.0F);
    }

    server.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // A checkpoint sidecar must exist (proves the snapshot truncated the WAL).
  ASSERT_TRUE(fs::exists(snapshot_dir));
  bool sidecar_found = false;
  for (const auto& entry : fs::directory_iterator(snapshot_dir)) {
    if (entry.path().extension() == ".walseq") {
      sidecar_found = true;
    }
  }
  ASSERT_TRUE(sidecar_found);

  // --- Second server lifetime: brand-new server over the same dirs. ---
  {
    config::Config config = MakeConfig(snapshot_dir, wal_dir);
    NvecdServer server(config);
    // Start() drives the real recovery path: FindLatestSnapshot -> ReadSnapshotV1
    // -> WAL Open -> Replay(from = checkpoint + 1) -> publish ctx.wal.
    ASSERT_TRUE(server.Start().has_value());
    const uint16_t port = server.GetPort();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Scope the client so it disconnects before Stop(), keeping shutdown fast.
    {
      TcpClient client("127.0.0.1", port);

      // Pre-snapshot state recovered from the snapshot. A SIM neighbor query never
      // returns the query id itself, so metadata recovery is verified by querying a
      // different item and applying the recovered status:active filter, which keeps
      // only item_a.
      EXPECT_NE(client.SendCommand("SIM item_b 10 using=vectors").find("OK"), std::string::npos);
      std::string filtered = client.SendCommand("SIM item_b 10 using=vectors filter=status:active");
      EXPECT_NE(filtered.find("OK"), std::string::npos);
      EXPECT_NE(filtered.find("item_a"), std::string::npos);

      // Post-snapshot state recovered from the WAL tail.
      std::string sim_c = client.SendCommand("SIM item_c 10 using=vectors");
      EXPECT_NE(sim_c.find("OK"), std::string::npos);
      EXPECT_FLOAT_EQ(ParseEventScore(client.SendCommand("SIM item_a 10 using=events"), "item_c"), post_snapshot_score);

      // CRITICAL: the pre-snapshot co-occurrence score is single-counted. The
      // snapshot contributed it once; replay started at checkpoint + 1, so the
      // pre-snapshot events were not re-applied.
      float recovered_score = ParseEventScore(client.SendCommand("SIM item_a 10 using=events"), "item_b");
      EXPECT_FLOAT_EQ(recovered_score, pre_snapshot_score);

      // Every record at or below the checkpoint was applied zero times.
      const std::string info = client.SendCommand("INFO");
      EXPECT_NE(info.find("wal_replay_records_skipped: 0"), std::string::npos) << info;
      EXPECT_EQ(client.SendCommand("SIM item_d 10 using=vectors").find("OK"), std::string::npos);
    }

    server.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  fs::remove_all(root);
}

namespace {

/// Start a server over @p config, send @p commands in order, and stop it.
/// Returns the responses, or an empty vector when startup failed.
std::vector<std::string> RunLifetime(const config::Config& config, const std::vector<std::string>& commands) {
  NvecdServer server(config);
  if (!server.Start()) {
    return {};
  }
  std::vector<std::string> responses;
  {
    TcpClient client("127.0.0.1", server.GetPort());
    for (const auto& command : commands) {
      responses.push_back(client.SendCommand(command));
    }
  }
  server.Stop();
  return responses;
}

fs::path MakeRoot(const std::string& name) {
  const auto root = fs::temp_directory_path() / (name + "_" + std::to_string(static_cast<unsigned>(::getpid())));
  fs::remove_all(root);
  fs::create_directories(root / "snapshots");
  fs::create_directories(root / "wal");
  return root;
}

}  // namespace

TEST(WalServerRestartRecovery, WritesAfterIdleRestartAndSnapshotSurviveTheNextRestart) {
  const auto root = MakeRoot("nvecd_wal_idle_restart");
  const auto config = MakeConfig((root / "snapshots").string(), (root / "wal").string());

  auto first = RunLifetime(config, {"VECSET before 1 0"});
  ASSERT_EQ(first.size(), 1U);
  ASSERT_EQ(first[0].rfind("OK", 0), 0U) << first[0];

  // An idle lifetime leaves only an empty current segment; its snapshot then
  // truncates every segment that held a record.
  auto idle = RunLifetime(config, {"DUMP SAVE idle.nvec"});
  ASSERT_EQ(idle.size(), 1U);
  ASSERT_NE(idle[0].find("OK DUMP_SAVED"), std::string::npos) << idle[0];

  auto after = RunLifetime(config, {"VECSET after 0 1"});
  ASSERT_EQ(after.size(), 1U);
  ASSERT_EQ(after[0].rfind("OK", 0), 0U) << after[0];

  auto recovered = RunLifetime(config, {"SIMV 10 1 0", "INFO"});
  ASSERT_EQ(recovered.size(), 2U) << "restart after the idle snapshot failed";
  EXPECT_NE(recovered[0].find("before"), std::string::npos) << recovered[0];
  EXPECT_NE(recovered[0].find("after"), std::string::npos) << recovered[0];
  EXPECT_NE(recovered[1].find("vector_count: 2"), std::string::npos) << recovered[1];

  fs::remove_all(root);
}

TEST(WalServerRestartRecovery, UnrecoverableSnapshotNameNeverTruncatesTheWal) {
  const auto root = MakeRoot("nvecd_wal_unrecoverable_name");
  const auto config = MakeConfig((root / "snapshots").string(), (root / "wal").string());

  auto responses = RunLifetime(config, {"VECSET base 1 0", "DUMP SAVE base.nvec", "VECSET middle 0 1",
                                        "DUMP SAVE backup.bak", "VECSET tail 1 1"});
  ASSERT_EQ(responses.size(), 5U);
  ASSERT_NE(responses[1].find("OK DUMP_SAVED"), std::string::npos) << responses[1];
  EXPECT_EQ(responses[3].rfind("ERROR", 0), 0U) << responses[3];
  EXPECT_NE(responses[3].find(".nvec or .dmp"), std::string::npos) << responses[3];
  EXPECT_FALSE(fs::exists(root / "snapshots" / "backup.bak"));
  EXPECT_FALSE(fs::exists(root / "snapshots" / "backup.bak.walseq"));

  auto recovered = RunLifetime(config, {"SIMV 10 1 0", "INFO"});
  ASSERT_EQ(recovered.size(), 2U) << "restart after the rejected save failed";
  for (const char* id : {"base", "middle", "tail"}) {
    EXPECT_NE(recovered[0].find(id), std::string::npos) << id << " missing: " << recovered[0];
  }
  EXPECT_NE(recovered[1].find("vector_count: 3"), std::string::npos) << recovered[1];

  fs::remove_all(root);
}

TEST(WalServerRestartRecovery, IgnoresTempAndFallsBackFromNewestCorruptSnapshot) {
  const auto root =
      fs::temp_directory_path() / ("nvecd_snapshot_fallback_" + std::to_string(static_cast<unsigned>(::getpid())));
  fs::remove_all(root);
  const std::string snapshot_dir = (root / "snapshots").string();
  const std::string wal_dir = (root / "wal").string();
  fs::create_directories(snapshot_dir);
  fs::create_directories(wal_dir);

  config::Config snapshot_config;
  config::EventsConfig events_config;
  config::VectorsConfig vectors_config;
  events::EventStore event_store(events_config);
  events::CoOccurrenceIndex co_index;
  vectors::VectorStore vector_store(vectors_config);
  ASSERT_TRUE(vector_store.SetVector("valid_item", {1.0F, 0.0F}).has_value());

  const fs::path valid_path = root / "snapshots" / "old.nvec";
  ASSERT_TRUE(
      storage::snapshot_v1::WriteSnapshotV1(valid_path.string(), snapshot_config, event_store, co_index, vector_store)
          .has_value());
  const fs::path corrupt_path = root / "snapshots" / "new.nvec";
  fs::copy_file(valid_path, corrupt_path);
  {
    std::fstream corrupt(corrupt_path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(corrupt.is_open());
    corrupt.seekg(100);
    char byte = 0;
    corrupt.read(&byte, 1);
    corrupt.clear();
    corrupt.seekp(100);
    byte ^= static_cast<char>(0x5A);
    corrupt.write(&byte, 1);
  }
  // This represents an unpublished writer artifact and must never be scanned.
  fs::copy_file(valid_path, root / "snapshots" / "newer.nvec.tmp.abcd");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(valid_path, now - std::chrono::seconds(3));
  fs::last_write_time(corrupt_path, now - std::chrono::seconds(2));
  fs::last_write_time(root / "snapshots" / "newer.nvec.tmp.abcd", now - std::chrono::seconds(1));

  config::Config server_config = MakeConfig(snapshot_dir, wal_dir);
  server_config.wal.enabled = false;
  NvecdServer server(server_config);
  auto started = server.Start();
  ASSERT_TRUE(started.has_value()) << started.error().message();
  {
    TcpClient client("127.0.0.1", server.GetPort());
    const std::string response = client.SendCommand("SIMV 10 1 0");
    EXPECT_NE(response.find("valid_item"), std::string::npos) << response;
  }
  server.Stop();
  fs::remove_all(root);
}

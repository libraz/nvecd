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

#include "config/config.h"
#include "server/nvecd_server.h"

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

      // Lock-mode DUMP SAVE writes the checkpoint sidecar and truncates the WAL.
      ASSERT_NE(client.SendCommand("DUMP SAVE").find("OK"), std::string::npos);

      // Post-snapshot ops: a new co-occurring item and a new vector.
      ASSERT_EQ(client.SendCommand("EVENT ctx2 ADD item_a 70").find("OK"), 0U);
      ASSERT_EQ(client.SendCommand("EVENT ctx2 ADD item_c 60").find("OK"), 0U);
      ASSERT_EQ(client.SendCommand("VECSET item_c 0 1").find("OK"), 0U);
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
      EXPECT_GT(ParseEventScore(client.SendCommand("SIM item_a 10 using=events"), "item_c"), 0.0F);

      // CRITICAL: the pre-snapshot co-occurrence score is single-counted. The
      // snapshot contributed it once; replay started at checkpoint + 1, so the
      // pre-snapshot events were not re-applied.
      float recovered_score = ParseEventScore(client.SendCommand("SIM item_a 10 using=events"), "item_b");
      EXPECT_FLOAT_EQ(recovered_score, pre_snapshot_score);
    }

    server.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  fs::remove_all(root);
}

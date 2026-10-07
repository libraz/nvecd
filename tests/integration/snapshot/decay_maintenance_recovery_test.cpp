/**
 * @file decay_maintenance_recovery_test.cpp
 * @brief Durability of co-occurrence decay maintenance through the real server
 *
 * With the WAL, a decay pass is a log record replayed at restart. Without it,
 * the pass is persisted by a fork snapshot taken outside the write gates, and a
 * failed snapshot leaves the server writable.
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "config/config.h"
#include "server/nvecd_server.h"
#include "test_tcp_client.h"

namespace {

namespace fs = std::filesystem;

constexpr int kDecayIntervalSec = 3;
constexpr double kDecayAlpha = 0.5;

nvecd::config::Config MakeConfig(const fs::path& root, bool wal_enabled) {
  nvecd::config::Config config;
  config.api.tcp.bind = "127.0.0.1";
  config.api.tcp.port = 0;
  config.api.http.enable = false;
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.perf.thread_pool_size = 2;
  config.perf.max_connections = 8;
  config.events.decay_interval_sec = kDecayIntervalSec;
  config.events.decay_alpha = kDecayAlpha;
  config.snapshot.dir = (root / "snapshots").string();
  // Maintenance snapshots fork regardless of the DUMP SAVE mode.
  config.snapshot.mode = "lock";
  config.snapshot.interval_sec = 0;
  config.wal.enabled = wal_enabled;
  config.wal.dir = (root / "wal").string();
  config.wal.sync_on_write = true;
  return config;
}

fs::path MakeRoot(const std::string& name) {
  const auto root = fs::temp_directory_path() / (name + "_" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "snapshots");
  fs::create_directories(root / "wal");
  return root;
}

/// Score of @p neighbor in a "SIM <id> <k> using=events" reply, or -1.
double EventScore(TcpClient& client, const std::string& id, const std::string& neighbor) {
  const std::string reply = client.SendCommand("SIM " + id + " 10 using=events");
  const auto pos = reply.find("\n" + neighbor + " ");
  if (pos == std::string::npos) {
    return -1.0;
  }
  return std::stod(reply.substr(pos + neighbor.size() + 2));
}

/// Wait for the first decay pass to lower the score below @p initial.
double WaitForDecay(TcpClient& client, double initial) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kDecayIntervalSec * 5);
  double score = initial;
  while (score >= initial && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    score = EventScore(client, "item_a", "item_b");
  }
  return score;
}

/// Wait until the fork writer reports a finished snapshot of @p filename.
std::string WaitForSnapshotOutcome(TcpClient& client, const std::string& filename) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  std::string status;
  while (std::chrono::steady_clock::now() < deadline) {
    status = client.SendCommand("DUMP STATUS");
    if (status.find(filename) != std::string::npos && status.find("status: in_progress") == std::string::npos) {
      return status;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return status;
}

void Ingest(TcpClient& client) {
  ASSERT_EQ(client.SendCommand("EVENT ctx1 ADD item_a 100").rfind("OK", 0), 0U);
  ASSERT_EQ(client.SendCommand("EVENT ctx1 ADD item_b 100").rfind("OK", 0), 0U);
}

double RecoveredScore(nvecd::config::Config config) {
  config.events.decay_interval_sec = 0;
  nvecd::server::NvecdServer server(config);
  if (!server.Start()) {
    return -2.0;
  }
  double score = -1.0;
  {
    TcpClient client("127.0.0.1", server.GetPort());
    score = EventScore(client, "item_a", "item_b");
  }
  server.Stop();
  return score;
}

}  // namespace

TEST(DecayMaintenanceRecovery, WalOffDecayIsPersistedByAForkSnapshot) {
  const auto root = MakeRoot("nvecd_decay_wal_off");
  const auto config = MakeConfig(root, /*wal_enabled=*/false);

  double decayed = -1.0;
  {
    nvecd::server::NvecdServer server(config);
    ASSERT_TRUE(server.Start());
    TcpClient client("127.0.0.1", server.GetPort());
    Ingest(client);
    const double initial = EventScore(client, "item_a", "item_b");
    ASSERT_GT(initial, 0.0);

    decayed = WaitForDecay(client, initial);
    ASSERT_LT(decayed, initial);
    const std::string status = WaitForSnapshotOutcome(client, "maintenance.nvec");
    ASSERT_NE(status.find("status: completed"), std::string::npos) << status;
    client.Close();
    server.Stop();
  }

  EXPECT_DOUBLE_EQ(RecoveredScore(config), decayed);
  fs::remove_all(root);
}

TEST(DecayMaintenanceRecovery, WalOffSnapshotFailureLeavesTheServerWritable) {
  const auto root = MakeRoot("nvecd_decay_unwritable");
  const auto config = MakeConfig(root, /*wal_enabled=*/false);

  nvecd::server::NvecdServer server(config);
  ASSERT_TRUE(server.Start());
  ASSERT_EQ(::chmod((root / "snapshots").c_str(), 0500), 0);
  {
    TcpClient client("127.0.0.1", server.GetPort());
    Ingest(client);
    const double initial = EventScore(client, "item_a", "item_b");
    ASSERT_LT(WaitForDecay(client, initial), initial);
    const std::string status = WaitForSnapshotOutcome(client, "maintenance.nvec");
    EXPECT_NE(status.find("status: failed"), std::string::npos) << status;

    const std::string write = client.SendCommand("VECSET after_failure 1 0");
    EXPECT_EQ(write.rfind("OK", 0), 0U) << write;
  }
  server.Stop();
  ::chmod((root / "snapshots").c_str(), 0700);
  fs::remove_all(root);
}

TEST(DecayMaintenanceRecovery, WalOnDecayIsReplayedAfterRestart) {
  const auto root = MakeRoot("nvecd_decay_wal_on");
  const auto config = MakeConfig(root, /*wal_enabled=*/true);

  double decayed = -1.0;
  {
    nvecd::server::NvecdServer server(config);
    ASSERT_TRUE(server.Start());
    TcpClient client("127.0.0.1", server.GetPort());
    Ingest(client);
    const double initial = EventScore(client, "item_a", "item_b");
    ASSERT_GT(initial, 0.0);
    decayed = WaitForDecay(client, initial);
    ASSERT_LT(decayed, initial);
    client.Close();
    server.Stop();
  }

  EXPECT_DOUBLE_EQ(RecoveredScore(config), decayed);
  fs::remove_all(root);
}

TEST(DecayMaintenanceRecovery, WalOnZeroAlphaClearIsReplayedAndLaterWritesSurvive) {
  const auto root = MakeRoot("nvecd_decay_zero_alpha");
  auto config = MakeConfig(root, /*wal_enabled=*/true);
  config.events.decay_alpha = 0.0;  // documented: clears the index each pass

  {
    nvecd::server::NvecdServer server(config);
    ASSERT_TRUE(server.Start());
    TcpClient client("127.0.0.1", server.GetPort());
    Ingest(client);
    const double initial = EventScore(client, "item_a", "item_b");
    ASSERT_GT(initial, 0.0);
    ASSERT_LT(WaitForDecay(client, initial), 0.0);  // the pair is gone
    ASSERT_EQ(client.SendCommand("VECSET after_clear 1 0").rfind("OK", 0), 0U);
    client.Close();
    server.Stop();
  }

  // Recovery replays the alpha-0 record instead of stopping at it, so the
  // write acknowledged after it is still there.
  config.events.decay_interval_sec = 0;
  nvecd::server::NvecdServer restarted(config);
  ASSERT_TRUE(restarted.Start());
  {
    TcpClient client("127.0.0.1", restarted.GetPort());
    EXPECT_LT(EventScore(client, "item_a", "item_b"), 0.0);
    const std::string reply = client.SendCommand("SIM after_clear 1 using=vectors");
    EXPECT_EQ(reply.rfind("OK", 0), 0U) << reply;
  }
  restarted.Stop();
  fs::remove_all(root);
}

/**
 * @file snapshot_recovery_base_test.cpp
 * @brief Which snapshot startup recovery selects after an interrupted overwrite
 *        or a DUMP LOAD racing a background save
 */

#include <gtest/gtest.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/nvecd_server.h"
#include "storage/snapshot_format_v1.h"
#include "storage/snapshot_session.h"
#include "storage/wal_checkpoint.h"
#include "test_tcp_client.h"
#include "vectors/vector_store.h"

namespace {

namespace fs = std::filesystem;

nvecd::config::Config MakeConfig(const fs::path& root, const std::string& mode) {
  nvecd::config::Config config;
  config.api.tcp.bind = "127.0.0.1";
  config.api.tcp.port = 0;
  config.api.http.enable = false;
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.perf.thread_pool_size = 2;
  config.perf.max_connections = 8;
  config.events.decay_interval_sec = 0;
  config.snapshot.dir = (root / "snapshots").string();
  config.snapshot.mode = mode;
  config.snapshot.interval_sec = 0;
  config.wal.enabled = true;
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

bool IsOk(const std::string& response) {
  return response.rfind("OK", 0) == 0;
}

/// Poll DUMP STATUS until the fork writer leaves in_progress.
std::string WaitForForkSave(TcpClient& client) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  std::string status;
  while (std::chrono::steady_clock::now() < deadline) {
    status = client.SendCommand("DUMP STATUS");
    if (status.find("status: in_progress") == std::string::npos) {
      return status;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return status;
}

pid_t ParseChildPid(const std::string& status) {
  const auto pos = status.find("pid: ");
  if (pos == std::string::npos) {
    return -1;
  }
  return static_cast<pid_t>(std::stol(status.substr(pos + 5)));
}

}  // namespace

TEST(SnapshotRecoveryBase, CrashBetweenOverwriteAndSidecarRecoversThePreviousGeneration) {
  const auto root = MakeRoot("nvecd_overwrite_crash");
  const auto config = MakeConfig(root, "lock");
  const fs::path snapshot = root / "snapshots" / "base.nvec";

  {
    nvecd::server::NvecdServer server(config);
    ASSERT_TRUE(server.Start());
    TcpClient client("127.0.0.1", server.GetPort());
    ASSERT_TRUE(IsOk(client.SendCommand("VECSET kept 1 0")));
    ASSERT_TRUE(IsOk(client.SendCommand("DUMP SAVE base.nvec")));
    ASSERT_TRUE(IsOk(client.SendCommand("VECSET tail 0 1")));
    client.Close();
    server.Stop();
  }
  ASSERT_TRUE(nvecd::storage::ReadWalCheckpoint(snapshot.string()));

  // Replay a save to the same path that published its image and died before
  // its sidecar: the old sidecar now describes bytes that are gone.
  ASSERT_TRUE(nvecd::storage::PreserveRecoveryBase(snapshot.string()));
  {
    nvecd::config::EventsConfig events_config;
    nvecd::config::VectorsConfig vectors_config;
    nvecd::events::EventStore event_store(events_config);
    nvecd::events::CoOccurrenceIndex co_index;
    nvecd::vectors::VectorStore vector_store(vectors_config);
    ASSERT_TRUE(vector_store.SetVector("kept", {1.0F, 0.0F}));
    ASSERT_TRUE(vector_store.SetVector("tail", {0.0F, 1.0F}));
    ASSERT_TRUE(
        nvecd::storage::snapshot_v1::WriteSnapshotV1(snapshot.string(), config, event_store, co_index, vector_store));
  }
  ASSERT_FALSE(nvecd::storage::ReadWalCheckpoint(snapshot.string()));

  nvecd::server::NvecdServer recovered(config);
  auto started = recovered.Start();
  ASSERT_TRUE(started) << started.error().message();
  {
    TcpClient client("127.0.0.1", recovered.GetPort());
    const std::string results = client.SendCommand("SIMV 10 1 0");
    EXPECT_NE(results.find("kept"), std::string::npos) << results;
    EXPECT_NE(results.find("tail"), std::string::npos) << results;
    EXPECT_NE(client.SendCommand("INFO").find("vector_count: 2"), std::string::npos);

    // A completed save to the same path validates on its own again, so the
    // preserved generation is released.
    ASSERT_TRUE(IsOk(client.SendCommand("DUMP SAVE base.nvec")));
  }
  recovered.Stop();
  for (const auto& entry : fs::directory_iterator(root / "snapshots")) {
    EXPECT_EQ(entry.path().filename().string().find(".prev"), std::string::npos) << entry.path();
  }
  fs::remove_all(root);
}

TEST(SnapshotRecoveryBase, DumpLoadIsRefusedWhileABackgroundSaveIsWriting) {
  const auto root = MakeRoot("nvecd_load_during_fork");
  const auto config = MakeConfig(root, "fork");

  {
    nvecd::server::NvecdServer server(config);
    ASSERT_TRUE(server.Start());
    TcpClient client("127.0.0.1", server.GetPort());
    ASSERT_TRUE(IsOk(client.SendCommand("VECSET loaded 1 0")));
    ASSERT_TRUE(IsOk(client.SendCommand("DUMP SAVE rollback.nvec")));
    ASSERT_NE(WaitForForkSave(client).find("status: completed"), std::string::npos);
    ASSERT_TRUE(IsOk(client.SendCommand("VECSET discarded 0 1")));

    // Hold a second save's child mid-write so the load overlaps it.
    ASSERT_TRUE(IsOk(client.SendCommand("DUMP SAVE stale.nvec")));
    const pid_t child = ParseChildPid(client.SendCommand("DUMP STATUS"));
    ASSERT_GT(child, 0);
    ASSERT_EQ(::kill(child, SIGSTOP), 0);
    const std::string refused = client.SendCommand("DUMP LOAD rollback.nvec");
    ASSERT_EQ(::kill(child, SIGCONT), 0);
    EXPECT_EQ(refused.rfind("ERROR", 0), 0U) << refused;
    EXPECT_NE(refused.find("background save"), std::string::npos) << refused;

    ASSERT_NE(WaitForForkSave(client).find("status: completed"), std::string::npos);
    ASSERT_TRUE(IsOk(client.SendCommand("DUMP LOAD rollback.nvec")));
    ASSERT_TRUE(IsOk(client.SendCommand("VECSET after 1 1")));
    client.Close();
    server.Stop();
  }

  // The loaded snapshot outranks the save that finished before it, and the
  // WAL tail after the load replays on top of it.
  nvecd::server::NvecdServer recovered(config);
  auto started = recovered.Start();
  ASSERT_TRUE(started) << started.error().message();
  {
    TcpClient client("127.0.0.1", recovered.GetPort());
    const std::string results = client.SendCommand("SIMV 10 1 0");
    EXPECT_NE(results.find("loaded"), std::string::npos) << results;
    EXPECT_NE(results.find("after"), std::string::npos) << results;
    EXPECT_EQ(results.find("discarded"), std::string::npos) << results;
  }
  recovered.Stop();
  fs::remove_all(root);
}

TEST(SnapshotRecoveryBase, AutoSnapshotsUnderConcurrentWritesLoseNoAcknowledgedWrite) {
  const auto root = MakeRoot("nvecd_auto_snapshot_writes");
  auto config = MakeConfig(root, "fork");
  config.snapshot.interval_sec = 1;
  config.snapshot.retain = 2;

  size_t acknowledged = 0;
  {
    nvecd::server::NvecdServer server(config);
    ASSERT_TRUE(server.Start());
    TcpClient client("127.0.0.1", server.GetPort());
    // Long enough for several scheduler forks to capture mid-stream.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3500);
    for (size_t index = 0; std::chrono::steady_clock::now() < until; ++index) {
      if (IsOk(client.SendCommand("VECSET auto-" + std::to_string(index) + " 1 0"))) {
        ++acknowledged;
      }
    }
    client.Close();
    server.Stop();
  }
  bool checkpointed = false;
  for (const auto& entry : fs::directory_iterator(root / "snapshots")) {
    checkpointed = checkpointed || entry.path().extension() == nvecd::storage::kWalCheckpointSuffix;
  }
  ASSERT_TRUE(checkpointed) << "no automatic snapshot completed";

  nvecd::server::NvecdServer recovered(config);
  auto started = recovered.Start();
  ASSERT_TRUE(started) << started.error().message();
  {
    TcpClient client("127.0.0.1", recovered.GetPort());
    const std::string info = client.SendCommand("INFO");
    const auto pos = info.find("vector_count: ");
    ASSERT_NE(pos, std::string::npos) << info;
    EXPECT_EQ(std::stoul(info.substr(pos + 14)), acknowledged);
  }
  recovered.Stop();
  fs::remove_all(root);
}

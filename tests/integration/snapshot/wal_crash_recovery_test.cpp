#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "config/config.h"
#include "server/nvecd_server.h"
#include "test_tcp_client.h"

namespace {

enum class CrashBoundary { kFirstSegment, kRotation, kCheckpoint };

nvecd::config::Config MakeConfig(const std::filesystem::path& root, CrashBoundary boundary) {
  nvecd::config::Config config;
  config.api.tcp.bind = "127.0.0.1";
  config.api.tcp.port = 0;
  config.api.http.enable = false;
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.perf.thread_pool_size = 2;
  config.perf.max_connections = 8;
  config.vectors.default_dimension = 3;
  config.events.decay_interval_sec = 3600;
  config.snapshot.dir = (root / "snapshots").string();
  config.snapshot.mode = "lock";
  config.snapshot.interval_sec = 0;
  config.wal.enabled = true;
  config.wal.dir = (root / "wal").string();
  config.wal.sync_on_write = true;
  config.wal.max_file_size = boundary == CrashBoundary::kRotation ? 64 : 1024 * 1024;
  return config;
}

bool Accepted(const std::string& response) {
  return response.rfind("OK", 0) == 0 || response.rfind("+OK", 0) == 0;
}

[[noreturn]] void RunUntilKilled(const nvecd::config::Config& config, CrashBoundary boundary, int ready_fd) {
  nvecd::server::NvecdServer server(config);
  if (!server.Start()) {
    _exit(2);
  }
  try {
    TcpClient client("127.0.0.1", server.GetPort());
    bool accepted = true;
    if (boundary == CrashBoundary::kRotation) {
      for (int index = 0; index < 12; ++index) {
        accepted = accepted && Accepted(client.SendCommand("VECSET rotation-" + std::to_string(index) + " 1 0 0"));
      }
    } else if (boundary == CrashBoundary::kCheckpoint) {
      accepted = Accepted(client.SendCommand("VECSET checkpoint-before 1 0 0"));
      accepted = accepted && Accepted(client.SendCommand("DUMP SAVE crash-checkpoint.nvec"));
      accepted = accepted && Accepted(client.SendCommand("VECSET checkpoint-after 0.9 0.1 0"));
    } else {
      accepted = Accepted(client.SendCommand("VECSET first-segment 1 0 0"));
    }
    if (!accepted || ::write(ready_fd, "A", 1) != 1) {
      _exit(3);
    }
  } catch (...) {
    _exit(4);
  }
  for (;;) {
    ::pause();
  }
}

TEST(WalCrashRecoveryTest, AcknowledgedWritesSurviveSigkillAtAllWalBoundaries) {
  const std::vector<CrashBoundary> boundaries = {CrashBoundary::kFirstSegment, CrashBoundary::kRotation,
                                                 CrashBoundary::kCheckpoint};
  for (size_t index = 0; index < boundaries.size(); ++index) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("nvecd-crash-recovery-" + std::to_string(::getpid()) + "-" + std::to_string(index));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "snapshots");
    std::filesystem::create_directories(root / "wal");
    const auto config = MakeConfig(root, boundaries[index]);

    int ready_pipe[2] = {-1, -1};
    ASSERT_EQ(::pipe(ready_pipe), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
      ::close(ready_pipe[0]);
      RunUntilKilled(config, boundaries[index], ready_pipe[1]);
    }
    ::close(ready_pipe[1]);
    char acknowledged = 0;
    ASSERT_EQ(::read(ready_pipe[0], &acknowledged, 1), 1) << "child failed before acknowledgement";
    ::close(ready_pipe[0]);
    ASSERT_EQ(acknowledged, 'A');
    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGKILL);

    nvecd::server::NvecdServer recovered(config);
    const auto started = recovered.Start();
    ASSERT_TRUE(started) << started.error().message();
    TcpClient client("127.0.0.1", recovered.GetPort());
    const std::string results = client.SendCommand("SIMV 20 1 0 0");
    if (boundaries[index] == CrashBoundary::kFirstSegment) {
      EXPECT_NE(results.find("first-segment"), std::string::npos);
    } else if (boundaries[index] == CrashBoundary::kRotation) {
      EXPECT_NE(results.find("rotation-11"), std::string::npos);
    } else {
      EXPECT_NE(results.find("checkpoint-before"), std::string::npos);
      EXPECT_NE(results.find("checkpoint-after"), std::string::npos);
    }
    recovered.Stop();
    std::filesystem::remove_all(root);
  }
}

}  // namespace

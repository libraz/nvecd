/**
 * @file main_logging_test.cpp
 * @brief The nvecd binary's handling of logging.file
 *
 * Runs the real binary: a configuration check leaves an existing log untouched,
 * an unopenable log file is a reported startup error rather than an abort, and
 * normal startup appends to the log instead of erasing it.
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr const char* kPreviousLog = "previous run log line\n";

/// An ephemeral loopback port that was free a moment ago.
int FreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  socklen_t length = sizeof(address);
  int port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
      ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
    port = ntohs(address.sin_port);
  }
  ::close(fd);
  return port;
}

class MainLoggingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("nvecd_main_logging_" + std::to_string(::getpid()));
    fs::remove_all(root_);
    fs::create_directories(root_ / "snapshots");
    fs::permissions(root_ / "snapshots", fs::perms::owner_all, fs::perm_options::replace);
  }
  void TearDown() override { fs::remove_all(root_); }

  fs::path WriteConfig(const fs::path& log_file) const {
    const fs::path config = root_ / "config.yaml";
    std::ofstream(config) << "api:\n  tcp:\n    bind: \"127.0.0.1\"\n    port: " << port_
                          << "\n  http:\n    enable: false\n"
                          << "snapshot:\n  dir: \"" << (root_ / "snapshots").string() << "\"\n"
                          << "logging:\n  file: \"" << log_file.string() << "\"\n";
    return config;
  }

  fs::path WritePreviousLog() const {
    const fs::path log = root_ / "nvecd.log";
    std::ofstream(log) << kPreviousLog;
    return log;
  }

  static std::string Read(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  /// Run the binary with @p args and return its raw wait status.
  static int Run(const std::vector<std::string>& args) {
    const pid_t child = ::fork();
    if (child == 0) {
      std::vector<char*> argv;
      argv.push_back(const_cast<char*>(NVECD_BINARY));
      for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
      }
      argv.push_back(nullptr);
      ::execv(NVECD_BINARY, argv.data());
      _exit(127);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    return status;
  }

  /// Whether the server accepts a TCP connection on port_.
  bool Accepting() const {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port_));
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return connected;
  }

  fs::path root_;
  int port_ = FreePort();
};

}  // namespace

TEST_F(MainLoggingTest, ConfigTestLeavesTheLogFileUntouched) {
  const auto log = WritePreviousLog();
  const int status = Run({"-t", "-c", WriteConfig(log).string()});
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(Read(log), kPreviousLog);
}

TEST_F(MainLoggingTest, UnopenableLogFileIsAReportedStartupError) {
  // The parent is a regular file, so the log can be neither created nor opened.
  std::ofstream(root_ / "not_a_directory") << "x";
  const int status = Run({"-c", WriteConfig(root_ / "not_a_directory" / "nvecd.log").string()});
  ASSERT_TRUE(WIFEXITED(status)) << "terminated by signal " << WTERMSIG(status);
  EXPECT_EQ(WEXITSTATUS(status), 1);
}

TEST_F(MainLoggingTest, StartupAppendsToTheExistingLog) {
  const auto log = WritePreviousLog();
  const auto config = WriteConfig(log);
  const pid_t child = ::fork();
  if (child == 0) {
    ::execl(NVECD_BINARY, NVECD_BINARY, "-c", config.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!Accepting() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ::kill(child, SIGTERM);
  int status = 0;
  ::waitpid(child, &status, 0);

  const std::string contents = Read(log);
  EXPECT_EQ(contents.rfind(kPreviousLog, 0), 0U) << contents;
  EXPECT_NE(contents.find("Server is running"), std::string::npos) << contents;
}

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "config/config.h"
#include "server/nvecd_server.h"

namespace {

int RunCli(uint16_t port, const char* password) {
  const pid_t child = ::fork();
  if (child == 0) {
    const int null_fd = ::open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      ::dup2(null_fd, STDOUT_FILENO);
      ::dup2(null_fd, STDERR_FILENO);
      ::close(null_fd);
    }
    ::setenv("NVECD_CLI_TEST_PASSWORD", password, 1);
    const std::string port_text = std::to_string(port);
    ::execl(NVECD_CLI_PATH, NVECD_CLI_PATH, "-h", "127.0.0.1", "-p", port_text.c_str(), "--password-env",
            "NVECD_CLI_TEST_PASSWORD", "VECSET", "cli-auth-item", "1", "0", "0", nullptr);
    _exit(127);
  }
  if (child < 0) {
    return -1;
  }
  int status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
    return -1;
  }
  return WEXITSTATUS(status);
}

TEST(NvecdCliTest, OneShotSecretAuthAndCommandUseOneConnection) {
  std::array<char, 40> directory_template{};
  const std::string template_text = "/tmp/nvecd-cli-test.XXXXXX";
  std::copy(template_text.begin(), template_text.end(), directory_template.begin());
  char* directory = ::mkdtemp(directory_template.data());
  ASSERT_NE(directory, nullptr);

  nvecd::config::Config config;
  config.api.tcp.port = 0;
  config.api.http.enable = false;
  config.security.requirepass = "correct-password";
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.vectors.default_dimension = 3;
  config.snapshot.dir = directory;
  config.snapshot.mode = "lock";

  {
    nvecd::server::NvecdServer server(config);
    const auto started = server.Start();
    ASSERT_TRUE(started) << started.error().message();
    ASSERT_GT(server.GetPort(), 0);

    EXPECT_NE(RunCli(server.GetPort(), "wrong-password"), 0);
    EXPECT_EQ(RunCli(server.GetPort(), "correct-password"), 0);
    server.Stop();
  }

  std::filesystem::remove_all(directory);
}

}  // namespace

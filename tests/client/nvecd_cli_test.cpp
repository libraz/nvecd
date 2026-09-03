#include <arpa/inet.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "canned_response_server.h"
#include "config/config.h"
#include "server/nvecd_server.h"

namespace {

struct CliRun {
  int exit_code = -1;
  std::string output;
};

// Run the CLI with the given arguments, feeding it @p input on stdin and
// collecting stdout and stderr together. stdin reaches end of file once @p input
// is written, so an interactive session ends without needing a quit command.
CliRun RunCliArgs(const std::vector<std::string>& args, const std::string& input = "") {
  int pipe_fds[2] = {-1, -1};
  int input_fds[2] = {-1, -1};
  if (::pipe(pipe_fds) != 0) {
    return {};
  }
  if (::pipe(input_fds) != 0) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    return {};
  }

  const pid_t child = ::fork();
  if (child == 0) {
    ::close(pipe_fds[0]);
    ::close(input_fds[1]);
    ::dup2(input_fds[0], STDIN_FILENO);
    ::dup2(pipe_fds[1], STDOUT_FILENO);
    ::dup2(pipe_fds[1], STDERR_FILENO);
    ::close(input_fds[0]);
    ::close(pipe_fds[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(NVECD_CLI_PATH));
    for (const std::string& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(NVECD_CLI_PATH, argv.data());
    _exit(127);
  }
  ::close(pipe_fds[1]);
  ::close(input_fds[0]);
  if (child < 0) {
    ::close(pipe_fds[0]);
    ::close(input_fds[1]);
    return {};
  }
  if (!input.empty()) {
    (void)::write(input_fds[1], input.data(), input.size());
  }
  ::close(input_fds[1]);

  CliRun run;
  std::array<char, 512> buffer{};
  ssize_t received = 0;
  while ((received = ::read(pipe_fds[0], buffer.data(), buffer.size())) > 0) {
    run.output.append(buffer.data(), static_cast<size_t>(received));
  }
  ::close(pipe_fds[0]);

  int status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
    return run;
  }
  run.exit_code = WEXITSTATUS(status);
  return run;
}

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

TEST(NvecdCliTest, ARefusedDebugCommandDoesNotLatchDebugFraming) {
  // DEBUG mode changes how the next SIM response is framed: the CLI waits for a
  // "# DEBUG <n>" block after the result rows. Latching the mode on a refused
  // DEBUG ON makes it wait for a block the server never sends, so the next
  // search stalls until the receive deadline instead of printing its results.
  nvecd::testing::CannedResponseServer server({"(error) debug refused\r\n", "OK RESULTS 1\r\nitem 0.9000\r\n"});
  ASSERT_GT(server.Port(), 0);

  const CliRun run =
      RunCliArgs({"-h", "127.0.0.1", "-p", std::to_string(server.Port())}, "DEBUG ON\nSIM item 1 using=vectors\n");

  EXPECT_EQ(run.output.find("SERVER_TIMEOUT"), std::string::npos) << run.output;
  EXPECT_NE(run.output.find("item (score: 0.9)"), std::string::npos) << run.output;
}

TEST(NvecdCliTest, ASilentServerIsReportedAsATimeoutNotAsAGenericReceiveFailure) {
  // The CLI has to call the same condition a timeout that the client library
  // reports as a timeout error code. A receive deadline expires as EAGAIN on
  // some platforms and as ETIMEDOUT on others, so recognising only one of them
  // makes the two surfaces disagree about what happened.
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listener, 0);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Required for socket API
  ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ASSERT_EQ(::listen(listener, 1), 0);
  socklen_t address_length = sizeof(address);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Required for socket API
  ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length), 0);
  const uint16_t port = ntohs(address.sin_port);

  std::thread peer([listener]() {
    const int connection = ::accept(listener, nullptr, nullptr);
    if (connection >= 0) {
      std::array<char, 64> buffer{};
      (void)::recv(connection, buffer.data(), buffer.size(), 0);  // Take the command, answer nothing
      (void)::recv(connection, buffer.data(), buffer.size(), 0);  // Wait for the CLI to give up
      ::close(connection);
    }
    ::close(listener);
  });

  const CliRun run = RunCliArgs({"-h", "127.0.0.1", "-p", std::to_string(port), "INFO"});
  EXPECT_NE(run.output.find("SERVER_TIMEOUT"), std::string::npos) << run.output;
  peer.join();
}

TEST(NvecdCliTest, HostOptionAcceptsAHostnameAndNamesAResolutionFailure) {
  std::array<char, 40> directory_template{};
  const std::string template_text = "/tmp/nvecd-cli-host.XXXXXX";
  std::copy(template_text.begin(), template_text.end(), directory_template.begin());
  char* directory = ::mkdtemp(directory_template.data());
  ASSERT_NE(directory, nullptr);

  nvecd::config::Config config;
  config.api.tcp.port = 0;
  config.api.http.enable = false;
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.vectors.default_dimension = 3;
  config.snapshot.dir = directory;
  config.snapshot.mode = "lock";

  {
    nvecd::server::NvecdServer server(config);
    const auto started = server.Start();
    ASSERT_TRUE(started) << started.error().message();
    ASSERT_GT(server.GetPort(), 0);

    const std::string port_text = std::to_string(server.GetPort());

    // -h is documented as a hostname and the usage text's own example passes
    // "localhost", so a name has to connect exactly as a numeric address does.
    const CliRun by_name = RunCliArgs({"-h", "localhost", "-p", port_text, "INFO"});
    EXPECT_EQ(by_name.exit_code, 0) << by_name.output;
    const CliRun by_address = RunCliArgs({"-h", "127.0.0.1", "-p", port_text, "INFO"});
    EXPECT_EQ(by_address.exit_code, 0) << by_address.output;

    // The .invalid top-level domain is reserved precisely so it never resolves.
    const CliRun unresolvable = RunCliArgs({"-h", "nvecd-no-such-host.invalid", "-p", port_text, "INFO"});
    EXPECT_NE(unresolvable.exit_code, 0);
    EXPECT_NE(unresolvable.output.find("Failed to resolve host"), std::string::npos) << unresolvable.output;
    EXPECT_EQ(unresolvable.output.find("Invalid address"), std::string::npos) << unresolvable.output;

    server.Stop();
  }

  std::filesystem::remove_all(directory);
}

}  // namespace

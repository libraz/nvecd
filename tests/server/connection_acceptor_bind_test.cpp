/**
 * @file connection_acceptor_bind_test.cpp
 * @brief Bind failure classification for the TCP listener
 *
 * A listener that cannot bind must say why. A port already held by another
 * server and an address that does not resolve are different operational
 * problems with different fixes, so they must not collapse into one code.
 */

#include <gtest/gtest.h>

#include <string>

#include "server/connection_acceptor.h"
#include "server/server_types.h"
#include "server/thread_pool.h"
#include "utils/error.h"

namespace {

class ConnectionAcceptorBindTest : public ::testing::Test {
 protected:
  /// TCP config on an ephemeral port, so the test never depends on a fixed one.
  static nvecd::server::ServerConfig MakeTcpConfig() {
    nvecd::server::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.max_connections = 10;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)
    return config;
  }
};

TEST_F(ConnectionAcceptorBindTest, SecondAcceptorOnTheSamePortReportsAddressInUse) {
  nvecd::server::ThreadPool pool(2);
  nvecd::server::ConnectionAcceptor first(MakeTcpConfig(), &pool);
  first.SetConnectionHandler([](int) {});

  auto started = first.Start();
  ASSERT_TRUE(started.has_value()) << "Start failed: " << started.error().message();
  const uint16_t port = first.GetPort();
  ASSERT_GT(port, 0) << "Acceptor did not report a bound port";

  auto duplicate_config = MakeTcpConfig();
  duplicate_config.port = port;
  nvecd::server::ConnectionAcceptor duplicate(duplicate_config, &pool);
  duplicate.SetConnectionHandler([](int) {});

  auto conflict = duplicate.Start();
  ASSERT_FALSE(conflict.has_value()) << "Second acceptor bound port " << port << " already held by the first";
  EXPECT_EQ(conflict.error().code(), nvecd::utils::ErrorCode::kNetworkAddressInUse) << conflict.error().to_string();
  EXPECT_NE(conflict.error().message().find("another server is listening"), std::string::npos)
      << "Port conflict was not reported as such: " << conflict.error().message();
  EXPECT_FALSE(duplicate.IsRunning());

  EXPECT_TRUE(first.IsRunning());
  first.Stop();
}

TEST_F(ConnectionAcceptorBindTest, UnresolvableBindAddressIsNotReportedAsAPortConflict) {
  nvecd::server::ThreadPool pool(2);
  auto config = MakeTcpConfig();
  config.host = "not-an-address";
  nvecd::server::ConnectionAcceptor acceptor(config, &pool);
  acceptor.SetConnectionHandler([](int) {});

  auto started = acceptor.Start();
  ASSERT_FALSE(started.has_value());
  EXPECT_EQ(started.error().code(), nvecd::utils::ErrorCode::kNetworkInvalidBindAddress) << started.error().to_string();
  EXPECT_FALSE(acceptor.IsRunning());
}

}  // namespace

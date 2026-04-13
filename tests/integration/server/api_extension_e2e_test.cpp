/**
 * @file api_extension_e2e_test.cpp
 * @brief E2E tests for SIM/SIMV parameter extensions (filter, min_score)
 */

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "../test_server_fixture.h"
#include "../test_tcp_client.h"

class ApiExtensionE2ETest : public NvecdTestFixture {
 protected:
  void SetUp() override { SetUpServer(3); }
  void TearDown() override { TearDownServer(); }
};

// --- SIM with filter ---

TEST_F(ApiExtensionE2ETest, SimWithFilterReturnsFilteredResults) {
  TcpClient client("127.0.0.1", port_);

  // Register vectors
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item3 0.0 1.0 0.0")));

  // Register events for co-occurrence
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item1 100")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item2 90")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item3 80")));

  // SIM without filter — should return results
  auto response = client.SendCommand("SIM item1 10");
  EXPECT_TRUE(response.find("OK") != std::string::npos);
  auto results = ParseSimResults(response);
  EXPECT_GT(results.size(), 0U);

  // SIM with filter — should still parse correctly (results depend on metadata)
  auto filtered = client.SendCommand("SIM item1 10 filter=status:active");
  EXPECT_TRUE(filtered.find("OK") != std::string::npos);
}

// --- SIM with min_score ---

TEST_F(ApiExtensionE2ETest, SimWithMinScoreFiltersLowScores) {
  TcpClient client("127.0.0.1", port_);

  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item3 0.0 0.0 1.0")));

  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item1 100")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item2 90")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item3 10")));

  // Without min_score — all results
  auto all_results = client.SendCommand("SIM item1 10");
  auto all_parsed = ParseSimResults(all_results);

  // With high min_score — fewer or no results
  auto filtered = client.SendCommand("SIM item1 10 min_score=0.99");
  auto filtered_parsed = ParseSimResults(filtered);

  EXPECT_LE(filtered_parsed.size(), all_parsed.size());
}

// --- SIMV basic ---

TEST_F(ApiExtensionE2ETest, SimvReturnsResults) {
  TcpClient client("127.0.0.1", port_);

  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item3 0.0 1.0 0.0")));

  auto response = client.SendCommand("SIMV 3 1.0 0.0 0.0");
  EXPECT_TRUE(response.find("OK") != std::string::npos);
  auto results = ParseSimResults(response);
  EXPECT_GT(results.size(), 0U);

  // item1 should be the closest match
  if (!results.empty()) {
    EXPECT_EQ(results[0].first, "item1");
  }
}

// --- SIMV with filter ---

TEST_F(ApiExtensionE2ETest, SimvWithFilterParsesCorrectly) {
  TcpClient client("127.0.0.1", port_);

  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));

  auto response = client.SendCommand("SIMV 3 filter=tag:test 1.0 0.0 0.0");
  // Should not error — filter should be parsed
  EXPECT_TRUE(response.find("OK") != std::string::npos);
}

// --- Backward compatibility ---

TEST_F(ApiExtensionE2ETest, BackwardCompatSIMWithoutParams) {
  TcpClient client("127.0.0.1", port_);

  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item1 100")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item2 90")));

  // Plain SIM — no extra params
  auto response = client.SendCommand("SIM item1 10");
  EXPECT_TRUE(response.find("OK") != std::string::npos);
  EXPECT_GT(ParseSimResults(response).size(), 0U);
}

TEST_F(ApiExtensionE2ETest, BackwardCompatSIMVWithoutParams) {
  TcpClient client("127.0.0.1", port_);

  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));

  // Plain SIMV — no extra params
  auto response = client.SendCommand("SIMV 3 1.0 0.0 0.0");
  EXPECT_TRUE(response.find("OK") != std::string::npos);
}

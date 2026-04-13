/**
 * @file api_extension_e2e_test.cpp
 * @brief E2E tests for SIM/SIMV parameter extensions (filter, min_score)
 */

#include <gtest/gtest.h>

#include <algorithm>
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
  // NOTE: The server has no METASET command, so metadata can only be set via
  // the C++ MetadataStore API. This test verifies that the filter parameter is
  // parsed and accepted without error, but cannot verify actual filtering
  // behavior at the TCP protocol level.
  TcpClient client("127.0.0.1", port_);

  // Register vectors
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item3 0.0 1.0 0.0")));

  // Register events for co-occurrence
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item1 100")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item2 90")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item3 80")));

  // SIM without filter — baseline
  auto response = client.SendCommand("SIM item1 10");
  EXPECT_TRUE(ContainsOK(response));
  auto baseline = ParseSimResults(response);
  EXPECT_GT(baseline.size(), 0U);

  // SIM with filter — verify the filter parameter is accepted (no error)
  auto filtered_response = client.SendCommand("SIM item1 10 filter=status:active");
  EXPECT_TRUE(ContainsOK(filtered_response));
  auto filtered = ParseSimResults(filtered_response);

  // Without metadata set, filter has no items to exclude, so results should
  // still be returned. This confirms the filter parameter was parsed correctly
  // and did not cause a command error.
  EXPECT_GT(filtered.size(), 0U);
}

// --- SIM with min_score ---

TEST_F(ApiExtensionE2ETest, SimWithMinScoreFiltersLowScores) {
  TcpClient client("127.0.0.1", port_);

  // Register vectors with varying similarity to item1
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item3 0.0 0.0 1.0")));

  // Events for co-occurrence (item3 has low weight to produce lower score)
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item1 100")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item2 90")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item3 10")));

  // Get all results without min_score
  auto all_response = client.SendCommand("SIM item1 10");
  EXPECT_TRUE(ContainsOK(all_response));
  auto all_parsed = ParseSimResults(all_response);
  ASSERT_GE(all_parsed.size(), 2U) << "Need at least 2 results to test filtering";

  // Verify results are sorted descending by score
  for (size_t i = 1; i < all_parsed.size(); ++i) {
    EXPECT_GE(all_parsed[i - 1].second, all_parsed[i].second)
        << "Results should be sorted by score descending";
  }

  // Find a threshold between the highest and lowest scores
  float highest_score = all_parsed.front().second;
  float lowest_score = all_parsed.back().second;
  ASSERT_GT(highest_score, lowest_score)
      << "Need distinct scores to test min_score filtering";

  // Set min_score to midpoint — should exclude the lowest-scored items
  float threshold = (highest_score + lowest_score) / 2.0F;
  std::string cmd =
      "SIM item1 10 min_score=" + std::to_string(threshold);
  auto filtered_response = client.SendCommand(cmd);
  EXPECT_TRUE(ContainsOK(filtered_response));
  auto filtered_parsed = ParseSimResults(filtered_response);

  // Strictly fewer results than unfiltered
  EXPECT_LT(filtered_parsed.size(), all_parsed.size())
      << "min_score=" << threshold << " should have excluded at least one result"
      << " (lowest score was " << lowest_score << ")";

  // All returned results must have score >= threshold
  for (const auto& [id, score] : filtered_parsed) {
    EXPECT_GE(score, threshold)
        << "Item " << id << " has score " << score
        << " which is below min_score=" << threshold;
  }

  // Verify that the lowest-scored item from the unfiltered results is absent
  const std::string& lowest_id = all_parsed.back().first;
  bool lowest_found = std::any_of(
      filtered_parsed.begin(), filtered_parsed.end(),
      [&lowest_id](const auto& p) { return p.first == lowest_id; });
  EXPECT_FALSE(lowest_found)
      << "Item " << lowest_id << " (score=" << lowest_score
      << ") should have been filtered out by min_score=" << threshold;
}

TEST_F(ApiExtensionE2ETest, SimWithMinScoreZeroReturnsAll) {
  TcpClient client("127.0.0.1", port_);

  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item3 0.0 0.0 1.0")));

  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item1 100")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item2 90")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item3 10")));

  // Without min_score (default is 0.0)
  auto default_response = client.SendCommand("SIM item1 10");
  EXPECT_TRUE(ContainsOK(default_response));
  auto default_results = ParseSimResults(default_response);

  // Explicit min_score=0.0 should return the same results
  auto zero_response = client.SendCommand("SIM item1 10 min_score=0.0");
  EXPECT_TRUE(ContainsOK(zero_response));
  auto zero_results = ParseSimResults(zero_response);

  EXPECT_EQ(default_results.size(), zero_results.size())
      << "min_score=0.0 should return the same number of results as default";

  // Verify same items are returned
  for (size_t i = 0; i < default_results.size() && i < zero_results.size(); ++i) {
    EXPECT_EQ(default_results[i].first, zero_results[i].first);
  }
}

TEST_F(ApiExtensionE2ETest, SimWithMinScoreAboveMaxReturnsNone) {
  TcpClient client("127.0.0.1", port_);

  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item3 0.0 0.0 1.0")));

  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item1 100")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item2 90")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item3 10")));

  // min_score=1.01 is above the maximum possible normalized score (1.0),
  // so no results should pass the filter
  auto response = client.SendCommand("SIM item1 10 min_score=1.01");
  EXPECT_TRUE(ContainsOK(response));
  auto results = ParseSimResults(response);
  EXPECT_EQ(results.size(), 0U)
      << "min_score=1.01 should return no results since scores are in [0, 1]";
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
  // NOTE: The server has no METASET command, so metadata can only be set via
  // the C++ MetadataStore API. This test verifies that the filter parameter is
  // parsed and accepted without error at the TCP protocol level, but cannot
  // verify actual filtering behavior.
  TcpClient client("127.0.0.1", port_);

  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));

  auto response = client.SendCommand("SIMV 3 filter=tag:test 1.0 0.0 0.0");
  // Filter should be parsed without error
  EXPECT_TRUE(ContainsOK(response));
  auto results = ParseSimResults(response);
  // Without metadata, filter has nothing to exclude — results should still come back
  EXPECT_GT(results.size(), 0U);
}

// --- SIMV with min_score ---

TEST_F(ApiExtensionE2ETest, SimvWithMinScoreFiltersLowScores) {
  TcpClient client("127.0.0.1", port_);

  // Register vectors at varying distances from query [1.0, 0.0, 0.0]
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));   // identical
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));   // close
  EXPECT_TRUE(ContainsOK(client.SendCommand("VECSET item3 0.0 0.0 1.0")));   // orthogonal

  // Get all results without min_score
  auto all_response = client.SendCommand("SIMV 10 1.0 0.0 0.0");
  EXPECT_TRUE(ContainsOK(all_response));
  auto all_parsed = ParseSimResults(all_response);
  ASSERT_GE(all_parsed.size(), 2U) << "Need at least 2 results to test filtering";

  // Verify results are sorted descending by score
  for (size_t i = 1; i < all_parsed.size(); ++i) {
    EXPECT_GE(all_parsed[i - 1].second, all_parsed[i].second)
        << "Results should be sorted by score descending";
  }

  float highest_score = all_parsed.front().second;
  float lowest_score = all_parsed.back().second;
  ASSERT_GT(highest_score, lowest_score)
      << "Need distinct scores to test min_score filtering";

  // Set threshold between highest and lowest
  float threshold = (highest_score + lowest_score) / 2.0F;
  std::string cmd =
      "SIMV 10 min_score=" + std::to_string(threshold) + " 1.0 0.0 0.0";
  auto filtered_response = client.SendCommand(cmd);
  EXPECT_TRUE(ContainsOK(filtered_response));
  auto filtered_parsed = ParseSimResults(filtered_response);

  // Strictly fewer results
  EXPECT_LT(filtered_parsed.size(), all_parsed.size())
      << "min_score=" << threshold << " should have excluded at least one result"
      << " (lowest score was " << lowest_score << ")";

  // All returned results must have score >= threshold
  for (const auto& [id, score] : filtered_parsed) {
    EXPECT_GE(score, threshold)
        << "Item " << id << " has score " << score
        << " which is below min_score=" << threshold;
  }

  // Verify the lowest-scored item is absent
  const std::string& lowest_id = all_parsed.back().first;
  bool lowest_found = std::any_of(
      filtered_parsed.begin(), filtered_parsed.end(),
      [&lowest_id](const auto& p) { return p.first == lowest_id; });
  EXPECT_FALSE(lowest_found)
      << "Item " << lowest_id << " (score=" << lowest_score
      << ") should have been filtered out by min_score=" << threshold;
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

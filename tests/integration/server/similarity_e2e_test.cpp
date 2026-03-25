/**
 * @file similarity_e2e_test.cpp
 * @brief End-to-end tests for similarity search correctness via TCP
 *
 * Validates ordering, ranking, edge cases, and error handling for
 * SIM (events, vectors, fusion) and SIMV commands through the full
 * TCP server pipeline.
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../test_server_fixture.h"
#include "../test_tcp_client.h"

/**
 * @brief Fixture for similarity search E2E tests with 3-dimensional vectors
 */
class SimilarityE2ETest : public NvecdTestFixture {
 protected:
  void SetUp() override { SetUpServer(3); }
  void TearDown() override { TearDownServer(); }
};

// ---------------------------------------------------------------------------
// Test 1: Vector similarity ordering (cosine similarity)
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, VectorSimilarityOrdering) {
  TcpClient client("127.0.0.1", port_);

  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item3 0.0 0.0 1.0")));

  auto resp = client.SendCommand("SIM item1 10 using=vectors");
  ASSERT_TRUE(ContainsOK(resp));

  auto results = ParseSimResults(resp);
  ASSERT_GE(results.size(), 2u);

  // Find positions of item2 and item3
  int item2_pos = -1;
  int item3_pos = -1;
  for (size_t i = 0; i < results.size(); ++i) {
    if (results[i].first == "item2") {
      item2_pos = static_cast<int>(i);
    }
    if (results[i].first == "item3") {
      item3_pos = static_cast<int>(i);
    }
  }

  EXPECT_NE(item2_pos, -1) << "item2 should appear in results";
  EXPECT_NE(item3_pos, -1) << "item3 should appear in results";
  EXPECT_LT(item2_pos, item3_pos) << "item2 should rank before item3 (closer to item1 by cosine similarity)";
}

// ---------------------------------------------------------------------------
// Test 2: Event co-occurrence ordering
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, EventCoOccurrenceOrdering) {
  TcpClient client("127.0.0.1", port_);

  // A and B co-occur in ctx1 and ctx2; A and C co-occur only in ctx1
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD A 100")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD B 90")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD C 80")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx2 ADD A 100")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx2 ADD B 90")));

  auto resp = client.SendCommand("SIM A 10 using=events");
  ASSERT_TRUE(ContainsOK(resp));

  auto results = ParseSimResults(resp);

  // B co-occurs with A in 2 contexts, C in 1 context.
  // We must get at least 2 results containing both B and C.
  ASSERT_GE(results.size(), 2u) << "Expected at least 2 results";

  int b_pos = -1;
  int c_pos = -1;
  for (size_t i = 0; i < results.size(); ++i) {
    if (results[i].first == "B") {
      b_pos = static_cast<int>(i);
    }
    if (results[i].first == "C") {
      c_pos = static_cast<int>(i);
    }
  }

  ASSERT_NE(b_pos, -1) << "B not found in results";
  ASSERT_NE(c_pos, -1) << "C not found in results";
  EXPECT_LT(b_pos, c_pos) << "B should rank before C (co-occurs with A in 2 contexts vs 1)";
}

// ---------------------------------------------------------------------------
// Test 3: Fusion combines both signals
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, FusionCombinesBothSignals) {
  TcpClient client("127.0.0.1", port_);
  PopulateBasicData(client);

  auto resp = client.SendCommand("SIM item1 10 using=fusion");
  ASSERT_TRUE(ContainsOK(resp));

  int count = GetResultCount(resp);
  EXPECT_GT(count, 0) << "Fusion search should return results";
}

// ---------------------------------------------------------------------------
// Test 4: SIMV query vector ordering
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, SimvQueryVectorOrdering) {
  TcpClient client("127.0.0.1", port_);

  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET v1 1.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET v2 0.9 0.1 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET v3 0.0 0.0 1.0")));

  auto resp = client.SendCommand("SIMV 10 1.0 0.0 0.0");
  ASSERT_TRUE(ContainsOK(resp));

  auto results = ParseSimResults(resp);
  ASSERT_GE(results.size(), 1u);

  // v1 is the exact match for query [1,0,0], so it should rank first
  EXPECT_EQ(results[0].first, "v1") << "v1 should rank first as exact match for query vector";
}

// ---------------------------------------------------------------------------
// Test 5: Top-K limit respected
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, TopKLimitRespected) {
  TcpClient client("127.0.0.1", port_);

  // Add 20 vectors with slightly different values
  for (int i = 0; i < 20; ++i) {
    float x = 1.0f - static_cast<float>(i) * 0.04f;
    float y = static_cast<float>(i) * 0.04f;
    std::string cmd = "VECSET item_" + std::to_string(i) + " " + std::to_string(x) + " " + std::to_string(y) + " 0.0";
    ASSERT_TRUE(ContainsOK(client.SendCommand(cmd)));
  }

  auto resp = client.SendCommand("SIM item_0 5 using=vectors");
  ASSERT_TRUE(ContainsOK(resp));

  int count = GetResultCount(resp);
  EXPECT_LE(count, 5) << "Result count should not exceed top-K limit of 5";
}

// ---------------------------------------------------------------------------
// Test 6: Top-K zero
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, TopKZero) {
  TcpClient client("127.0.0.1", port_);

  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));

  auto resp = client.SendCommand("SIM item1 0 using=vectors");

  // Should return 0 results or an error
  if (ContainsOK(resp)) {
    int count = GetResultCount(resp);
    EXPECT_EQ(count, 0) << "Top-K 0 should return 0 results";
  } else {
    EXPECT_TRUE(ContainsError(resp)) << "Top-K 0 should return an error if not OK";
  }
}

// ---------------------------------------------------------------------------
// Test 7: SIM on non-existent ID
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, SimNonExistentId) {
  TcpClient client("127.0.0.1", port_);

  auto resp = client.SendCommand("SIM nonexistent 10 using=events");

  // Should return OK with 0 results or an error
  if (ContainsOK(resp)) {
    int count = GetResultCount(resp);
    EXPECT_EQ(count, 0) << "SIM on non-existent ID should return 0 results";
  } else {
    EXPECT_TRUE(ContainsError(resp)) << "SIM on non-existent ID should return an error if not OK";
  }
}

// ---------------------------------------------------------------------------
// Test 8: SIMV dimension mismatch
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, SimvDimensionMismatch) {
  TcpClient client("127.0.0.1", port_);

  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));

  // Send 2-dim vector to a 3-dim server
  auto resp = client.SendCommand("SIMV 10 1.0 2.0");
  EXPECT_TRUE(ContainsError(resp)) << "SIMV with wrong dimension should return an error";
}

// ---------------------------------------------------------------------------
// Test 9: SIM vectors mode with only events (no vectors)
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, SimVectorsOnlyEventsMode) {
  TcpClient client("127.0.0.1", port_);

  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item1 100")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item2 90")));

  auto resp = client.SendCommand("SIM item1 10 using=vectors");

  // Should handle gracefully: OK with 0 results or error
  if (ContainsOK(resp)) {
    int count = GetResultCount(resp);
    EXPECT_EQ(count, 0) << "SIM vectors with no vectors stored should return 0 results";
  } else {
    EXPECT_TRUE(ContainsError(resp));
  }
}

// ---------------------------------------------------------------------------
// Test 10: SIM events mode with only vectors (no events)
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, SimEventsOnlyVectorsMode) {
  TcpClient client("127.0.0.1", port_);

  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.9 0.1 0.0")));

  auto resp = client.SendCommand("SIM item1 10 using=events");

  // Should handle gracefully: OK with 0 results or error
  if (ContainsOK(resp)) {
    int count = GetResultCount(resp);
    EXPECT_EQ(count, 0) << "SIM events with no events stored should return 0 results";
  } else {
    EXPECT_TRUE(ContainsError(resp));
  }
}

// ---------------------------------------------------------------------------
// Test 11: Vector overwrite updates similarity results
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, VectorOverwrite) {
  TcpClient client("127.0.0.1", port_);

  // Set item1 to [1,0,0], then overwrite to [0,1,0]
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item1 0.0 1.0 0.0")));

  // item2 is close to the new item1 [0,1,0]
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.0 0.9 0.1")));

  auto resp = client.SendCommand("SIM item1 10 using=vectors");
  ASSERT_TRUE(ContainsOK(resp));

  auto results = ParseSimResults(resp);
  ASSERT_GE(results.size(), 1u);

  // item2 should be the closest to the overwritten item1 [0,1,0]
  EXPECT_EQ(results[0].first, "item2") << "item2 should be closest to overwritten item1 vector [0,1,0]";
}

// ---------------------------------------------------------------------------
// Test 12: EVENT SET via TCP
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, EventSetViaTCP) {
  TcpClient client("127.0.0.1", port_);

  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item1 100")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item2 50")));

  // SET updates item2's score
  auto resp = client.SendCommand("EVENT ctx1 SET item2 200");
  EXPECT_TRUE(ContainsOK(resp)) << "EVENT SET should return OK";

  // Verify search still works after SET
  resp = client.SendCommand("SIM item1 10 using=events");
  EXPECT_TRUE(ContainsOK(resp)) << "SIM should succeed after EVENT SET";
}

// ---------------------------------------------------------------------------
// Test 13: EVENT DEL via TCP
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, EventDelViaTCP) {
  TcpClient client("127.0.0.1", port_);

  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item1 100")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx1 ADD item2 90")));

  auto resp = client.SendCommand("EVENT ctx1 DEL item2");
  EXPECT_TRUE(ContainsOK(resp)) << "EVENT DEL should return OK";
}

// ---------------------------------------------------------------------------
// Test 14: Empty store search should not crash
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, EmptyStoreSearch) {
  TcpClient client("127.0.0.1", port_);

  auto resp = client.SendCommand("SIM nonexistent 10 using=events");

  // Should not crash; either OK with 0 results or ERROR
  EXPECT_FALSE(resp.empty()) << "Server should respond (not crash)";
  EXPECT_TRUE(ContainsOK(resp) || ContainsError(resp)) << "Response should be OK or ERROR, got: " + resp;
}

// ---------------------------------------------------------------------------
// Test 15: Empty store SIMV should not crash
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, EmptyStoreSimv) {
  TcpClient client("127.0.0.1", port_);

  auto resp = client.SendCommand("SIMV 10 1.0 0.0 0.0");

  // Should not crash; either OK with 0 results or ERROR
  EXPECT_FALSE(resp.empty()) << "Server should respond (not crash)";
  EXPECT_TRUE(ContainsOK(resp) || ContainsError(resp)) << "Response should be OK or ERROR, got: " + resp;
}

// ---------------------------------------------------------------------------
// Test 16: Single item store (no peers to compare against)
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, SingleItemStore) {
  TcpClient client("127.0.0.1", port_);

  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET only1 1.0 0.0 0.0")));

  auto resp = client.SendCommand("SIM only1 10 using=vectors");
  ASSERT_TRUE(ContainsOK(resp));

  int count = GetResultCount(resp);
  EXPECT_EQ(count, 0) << "Single item store should return 0 results (no other items to compare)";
}

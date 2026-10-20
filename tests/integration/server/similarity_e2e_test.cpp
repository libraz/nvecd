/**
 * @file similarity_e2e_test.cpp
 * @brief End-to-end tests for similarity search correctness via TCP
 *
 * Validates ordering, ranking, edge cases, and error handling for
 * SIM (events, vectors, fusion) and SIMV commands through the full
 * TCP server pipeline.
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
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
  auto resp = client.SendCommand("EVENT ctx1 SET item2 100");
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

  EXPECT_EQ(resp, "OK RESULTS 0\r\n");
}

// ---------------------------------------------------------------------------
// Test 15: Empty store SIMV should not crash
// ---------------------------------------------------------------------------

TEST_F(SimilarityE2ETest, EmptyStoreSimv) {
  TcpClient client("127.0.0.1", port_);

  auto resp = client.SendCommand("SIMV 10 1.0 0.0 0.0");

  EXPECT_EQ(resp, "OK RESULTS 0\r\n");
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

// ---------------------------------------------------------------------------
// Test 17: ANN index covers snapshot and WAL-tail vectors after a restart
// ---------------------------------------------------------------------------

namespace {

bool ReplyListsAll(const std::string& reply, const std::vector<std::string>& ids) {
  for (const auto& id : ids) {
    if (reply.find(id) == std::string::npos) {
      return false;
    }
  }
  return true;
}

/// Restart the server over its snapshot and WAL and expect SIMV to see every vector.
void ExpectAnnCoversRecoveredVectors(const std::string& index_type) {
  const auto root = fs::temp_directory_path() /
                    ("nvecd_ann_restart_" + index_type + "_" + std::to_string(static_cast<unsigned>(::getpid())));
  fs::remove_all(root);
  fs::create_directories(root / "snapshots");
  fs::create_directories(root / "wal");

  nvecd::config::Config config;
  config.api.tcp.bind = "127.0.0.1";
  config.api.tcp.port = 0;
  config.network.allow_cidrs = {"127.0.0.1/32"};
  config.perf.max_connections = 10;
  config.perf.thread_pool_size = 2;
  config.snapshot.dir = (root / "snapshots").string();
  config.snapshot.mode = "lock";
  config.snapshot.interval_sec = 0;
  config.wal.enabled = true;
  config.wal.dir = (root / "wal").string();
  config.wal.sync_on_write = true;
  config.vectors.default_dimension = 3;
  config.events.ctx_buffer_size = 100;
  config.similarity.default_top_k = 10;
  config.similarity.max_top_k = 100;
  config.similarity.index_type = index_type;
  // Small IVF geometry so a handful of vectors trains and seals; probing every
  // cell makes the result exact.
  config.similarity.ivf_nlist = 2;
  config.similarity.ivf_nprobe = 2;
  config.similarity.ivf_train_threshold = 4;
  config.similarity.ivf_seal_threshold = 4;

  const std::vector<std::string> snapshot_ids = {"snap_a", "snap_b", "snap_c", "snap_d"};
  const std::vector<std::string> tail_ids = {"tail_a", "tail_b"};

  {
    nvecd::server::NvecdServer server(config);
    ASSERT_TRUE(server.Start().has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TcpClient client("127.0.0.1", server.GetPort());
    ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET snap_a 1.0 0.0 0.0")));
    ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET snap_b 0.9 0.1 0.0")));
    ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET snap_c 0.8 0.2 0.0")));
    ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET snap_d 0.0 1.0 0.0")));
    ASSERT_NE(client.SendCommand("DUMP SAVE").find("OK"), std::string::npos);
    ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET tail_a 0.95 0.05 0.0")));
    ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET tail_b 0.0 0.0 1.0")));
    client.Close();
    server.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  {
    nvecd::server::NvecdServer server(config);
    ASSERT_TRUE(server.Start().has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TcpClient client("127.0.0.1", server.GetPort());

    // IVF trains and seals in the background after the rebuild, so allow it time.
    std::string reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
      reply = client.SendCommand("SIMV 10 1.0 0.0 0.0");
      if (ReplyListsAll(reply, snapshot_ids) && ReplyListsAll(reply, tail_ids)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    EXPECT_TRUE(ReplyListsAll(reply, snapshot_ids)) << index_type << " lost snapshot vectors: " << reply;
    EXPECT_TRUE(ReplyListsAll(reply, tail_ids)) << index_type << " lost WAL-tail vectors: " << reply;

    // A write after recovery goes through the incremental path onto the rebuilt index.
    ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET post_restart 0.99 0.01 0.0")));
    reply = client.SendCommand("SIMV 10 1.0 0.0 0.0");
    EXPECT_TRUE(ReplyListsAll(reply, snapshot_ids)) << index_type << ": " << reply;
    EXPECT_TRUE(ReplyListsAll(reply, tail_ids)) << index_type << ": " << reply;
    EXPECT_NE(reply.find("post_restart"), std::string::npos) << index_type << ": " << reply;

    client.Close();
    server.Stop();
  }

  fs::remove_all(root);
}

}  // namespace

TEST(AnnRestartRecoveryE2ETest, HnswIndexCoversSnapshotAndWalTailAfterRestart) {
  ExpectAnnCoversRecoveredVectors("hnsw");
}

TEST(AnnRestartRecoveryE2ETest, IvfIndexCoversSnapshotAndWalTailAfterRestart) {
  ExpectAnnCoversRecoveredVectors("ivf");
}

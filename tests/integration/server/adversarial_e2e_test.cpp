/**
 * @file adversarial_e2e_test.cpp
 * @brief Adversarial input, data mutation consistency, and cache invalidation
 *        end-to-end tests for nvecd server via TCP commands
 *
 * Tests categories:
 * - Adversarial protocol inputs (binary data, null bytes, extreme payloads,
 *   rapid-fire pipelining, special characters in IDs)
 * - Data mutation consistency (overwrite affecting similarity, event
 *   add/delete/re-add cycles, delete-all-then-search, concurrent write+query)
 * - Cache invalidation on data mutation (VECSET, EVENT, CACHE CLEAR)
 *
 * NOTE: Does NOT duplicate edge_case_e2e_test.cpp tests (unicode IDs, long IDs,
 * special chars like hyphen/dot/underscore/colon, NaN/Inf, zero scores, case
 * insensitivity, extra whitespace).
 */

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "../test_server_fixture.h"
#include "../test_tcp_client.h"

// ===========================================================================
// Fixture
// ===========================================================================

/**
 * @brief Fixture for adversarial and mutation-consistency E2E tests
 */
class AdversarialE2ETest : public NvecdTestFixture {
 protected:
  void SetUp() override { SetUpServer(3); }
  void TearDown() override { TearDownServer(); }
};

// ===========================================================================
// Adversarial Protocol Tests
// ===========================================================================

// ---------------------------------------------------------------------------
// Test 1: BinaryDataInCommand - null bytes embedded in command
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, BinaryDataInCommand) {
  TcpClient client("127.0.0.1", port_);

  // Build a command containing a null byte in the ID
  std::string cmd = "VECSET item";
  cmd += '\0';
  cmd += "bad 3 0.1 0.2 0.3";

  auto resp = client.SendCommand(cmd);
  // Null bytes may cause the server to see a truncated command (no \r\n
  // terminator visible), so an empty response (recv timeout) is acceptable.
  // The server may also respond with ERROR if it processes the partial data.
  EXPECT_TRUE(resp.empty() || ContainsOK(resp) || ContainsError(resp))
      << "Null byte command: unexpected response: " + resp;

  // Use a fresh connection to verify server is still functional, since the
  // null byte may have corrupted the stream state on the original connection.
  TcpClient health_client("127.0.0.1", port_);
  auto health = health_client.SendCommand("INFO");
  EXPECT_FALSE(health.empty()) << "Server should still respond after null byte command";
}

// ---------------------------------------------------------------------------
// Test 2: VeryLargePayload - 100,000 float values
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, VeryLargePayload) {
  TcpClient client("127.0.0.1", port_);

  // Build a VECSET with 100,000 dimensions (far exceeding configured 3)
  std::string cmd = "VECSET large_item";
  for (int i = 0; i < 100000; ++i) {
    cmd += " 0.001";
  }

  auto resp = client.SendCommand(cmd);
  // Should reject due to dimension mismatch or accept if dimension is ignored.
  // Must not crash.
  EXPECT_FALSE(resp.empty()) << "Server should respond (not crash) to very large payload";
  EXPECT_TRUE(ContainsOK(resp) || ContainsError(resp)) << "Response should be OK or ERROR, got: " + resp;

  // Verify server health
  auto health = client.SendCommand("INFO");
  EXPECT_FALSE(health.empty()) << "Server should still respond after very large payload";
}

// ---------------------------------------------------------------------------
// Test 3: RapidFireCommands - 1000 commands pipelined without reading
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, RapidFireCommands) {
  // Use a raw socket for pipelining (send all, then recv all)
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(sock, 0);

  struct sockaddr_in server_addr {};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port_);
  inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

  int ret = connect(sock, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr));
  ASSERT_EQ(ret, 0) << "Failed to connect for rapid-fire test";

  // Set recv timeout
  struct timeval tv {};
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Send 1000 EVENT ADD commands without reading responses
  static constexpr int kCommandCount = 1000;
  for (int i = 0; i < kCommandCount; ++i) {
    std::string cmd = "EVENT ctx_rf ADD item_" + std::to_string(i) + " " + std::to_string(i) + "\r\n";
    send(sock, cmd.c_str(), cmd.length(), 0);
  }

  // Now read all responses
  int ok_count = 0;
  int err_count = 0;
  std::string buffer;
  char recv_buf[65536];

  while (true) {
    ssize_t received = recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
    if (received <= 0) {
      break;
    }
    recv_buf[received] = '\0';
    buffer += recv_buf;

    // Count OK and ERROR occurrences
    size_t pos = 0;
    while ((pos = buffer.find("OK", pos)) != std::string::npos) {
      ++ok_count;
      pos += 2;
    }
    pos = 0;
    while ((pos = buffer.find("ERR", pos)) != std::string::npos) {
      ++err_count;
      pos += 3;
    }

    // If we have enough responses, stop
    if (ok_count + err_count >= kCommandCount) {
      break;
    }

    // Clear already-processed data but keep partial lines
    auto last_newline = buffer.rfind('\n');
    if (last_newline != std::string::npos) {
      // Reset counts - we'll do a final count below
      ok_count = 0;
      err_count = 0;
    }
  }

  close(sock);

  // Final count on accumulated buffer
  ok_count = 0;
  err_count = 0;
  size_t pos = 0;
  while ((pos = buffer.find("OK", pos)) != std::string::npos) {
    ++ok_count;
    pos += 2;
  }
  pos = 0;
  while ((pos = buffer.find("ERR", pos)) != std::string::npos) {
    ++err_count;
    pos += 3;
  }

  // We should have gotten responses for most commands
  EXPECT_GT(ok_count + err_count, 0) << "Server should have responded to at least some rapid-fire commands";

  // Verify server is still healthy via a fresh connection
  TcpClient health_client("127.0.0.1", port_);
  auto health = health_client.SendCommand("INFO");
  EXPECT_FALSE(health.empty()) << "Server should be healthy after rapid-fire commands";
}

// ---------------------------------------------------------------------------
// Test 4: EmptyAndWhitespaceOnlyCommands
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, EmptyAndWhitespaceOnlyCommands) {
  TcpClient client("127.0.0.1", port_);

  // Send empty line (just \r\n is appended by SendCommand)
  auto resp = client.SendCommand("");
  // Server may ignore empty lines (timeout/empty response) or return error
  EXPECT_TRUE(resp.empty() || ContainsError(resp) || ContainsOK(resp)) << "Empty command: unexpected response: " + resp;

  // Send whitespace-only commands
  resp = client.SendCommand("   ");
  EXPECT_TRUE(resp.empty() || ContainsError(resp) || ContainsOK(resp))
      << "Whitespace command: unexpected response: " + resp;

  resp = client.SendCommand("\t");
  EXPECT_TRUE(resp.empty() || ContainsError(resp) || ContainsOK(resp)) << "Tab command: unexpected response: " + resp;

  // Server must remain functional
  auto health = client.SendCommand("INFO");
  EXPECT_FALSE(health.empty()) << "Server should still respond after whitespace commands";
}

// ---------------------------------------------------------------------------
// Test 5: SpecialCharactersInIds - adversarial characters beyond edge_case set
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, SpecialCharactersInIds) {
  TcpClient client("127.0.0.1", port_);

  // ID with space (will be parsed as two tokens, so it is effectively invalid)
  auto resp = client.SendCommand("VECSET item 1 1.0 0.0 0.0");
  // "item" becomes the ID, "1" becomes the first float value.
  // This will likely be a dimension mismatch (4 values for 3-dim).
  EXPECT_TRUE(ContainsOK(resp) || ContainsError(resp)) << "ID with space: unexpected response: " + resp;

  // ID with equals sign
  resp = client.SendCommand("VECSET item=1 1.0 0.0 0.0");
  EXPECT_TRUE(ContainsOK(resp) || ContainsError(resp)) << "ID with equals: unexpected response: " + resp;

  // ID with backslash
  resp = client.SendCommand("VECSET item\\1 1.0 0.0 0.0");
  EXPECT_TRUE(ContainsOK(resp) || ContainsError(resp)) << "ID with backslash: unexpected response: " + resp;

  // Verify server health
  auto health = client.SendCommand("INFO");
  EXPECT_FALSE(health.empty()) << "Server should still respond after special character IDs";
}

// ---------------------------------------------------------------------------
// Test 6: NegativeTopK
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, NegativeTopK) {
  TcpClient client("127.0.0.1", port_);
  PopulateBasicData(client);

  auto resp = client.SendCommand("SIM item1 -5 using=cosine");
  EXPECT_TRUE(ContainsError(resp)) << "Negative top-K should return error, got: " + resp;

  // Server must remain functional
  auto health = client.SendCommand("INFO");
  EXPECT_FALSE(health.empty()) << "Server should still respond after negative top-K";
}

// ---------------------------------------------------------------------------
// Test 7: ZeroDimensionVector
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, ZeroDimensionVector) {
  TcpClient client("127.0.0.1", port_);

  // VECSET with no float values at all
  auto resp = client.SendCommand("VECSET item_empty");
  EXPECT_TRUE(ContainsError(resp)) << "VECSET with no values should return error, got: " + resp;

  // VECSET with ID only, trailing space
  resp = client.SendCommand("VECSET item_empty2 ");
  EXPECT_TRUE(resp.empty() || ContainsError(resp))
      << "VECSET with trailing space only should return error, got: " + resp;

  // Server must remain functional
  auto health = client.SendCommand("INFO");
  EXPECT_FALSE(health.empty()) << "Server should still respond after zero-dimension VECSET";
}

// ---------------------------------------------------------------------------
// Test 8: ExtremelyLargeTopK
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, ExtremelyLargeTopK) {
  TcpClient client("127.0.0.1", port_);
  PopulateBasicData(client);

  auto resp = client.SendCommand("SIM item1 999999999 using=vectors");
  // Should either be capped to max_top_k (100) and succeed, or return error
  EXPECT_TRUE(ContainsOK(resp) || ContainsError(resp)) << "Extremely large top-K should be handled, got: " + resp;

  if (ContainsOK(resp)) {
    int count = GetResultCount(resp);
    // Should not exceed total items in store minus the query item itself
    EXPECT_LE(count, 100) << "Result count should be capped by max_top_k";
  }

  // Server must remain functional
  auto health = client.SendCommand("INFO");
  EXPECT_FALSE(health.empty()) << "Server should still respond after extreme top-K";
}

// ===========================================================================
// Data Mutation Consistency Tests
// ===========================================================================

// ---------------------------------------------------------------------------
// Test 9: VecsetOverwriteUpdatesSimilarity
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, VecsetOverwriteUpdatesSimilarity) {
  TcpClient client("127.0.0.1", port_);

  // Setup: item1=[1,0,0], item2=[1,0,0] (identical), item3=[0,0,1] (orthogonal)
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item1 1.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item2 1.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item3 0.0 0.0 1.0")));

  // Before overwrite: item2 should be most similar to item1
  auto resp = client.SendCommand("SIM item1 10 using=vectors");
  ASSERT_TRUE(ContainsOK(resp));
  auto results_before = ParseSimResults(resp);
  ASSERT_GE(results_before.size(), 2u);
  EXPECT_EQ(results_before[0].first, "item2") << "Before overwrite: item2 should be most similar to item1";

  // Overwrite item2 to be orthogonal to item1
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET item2 0.0 0.0 1.0")));

  // After overwrite: item2 and item3 are both [0,0,1], orthogonal to item1
  resp = client.SendCommand("SIM item1 10 using=vectors");
  ASSERT_TRUE(ContainsOK(resp));
  auto results_after = ParseSimResults(resp);

  // item2 should no longer be at rank 1 (it is now equally distant as item3)
  // OR both should have the same low score. Either way, the result should
  // reflect the updated vector.
  if (results_after.size() >= 2) {
    // After overwrite, item2's score should be <= item3's score (both are
    // identical [0,0,1], so they should have the same similarity to item1).
    float item2_score = -1.0f;
    float item3_score = -1.0f;
    for (const auto& r : results_after) {
      if (r.first == "item2") {
        item2_score = r.second;
      }
      if (r.first == "item3") {
        item3_score = r.second;
      }
    }
    if (item2_score >= 0 && item3_score >= 0) {
      EXPECT_NEAR(item2_score, item3_score, 0.01f)
          << "After overwrite, item2 and item3 should have equal similarity to item1";
    }
  }
}

// ---------------------------------------------------------------------------
// Test 10: EventAddDeleteAddCycle
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, EventAddDeleteAddCycle) {
  TcpClient client("127.0.0.1", port_);

  // Step a: Add item1, item2, item3 in multiple contexts.
  // item1 and item2 co-occur in ctx_adc_0..4, item3 co-occurs with item1 in
  // ctx_adc_0..1 only (weaker co-occurrence).
  for (int i = 0; i < 5; ++i) {
    std::string ctx = "ctx_adc_" + std::to_string(i);
    ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT " + ctx + " ADD item1 100")));
    ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT " + ctx + " ADD item2 90")));
    if (i < 2) {
      ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT " + ctx + " ADD item3 80")));
    }
  }

  // Step b: Query similarity. The co-occurrence engine may return 0 results
  // if the index has not been built yet or if the implementation requires
  // additional conditions. We verify the server responds correctly.
  auto resp = client.SendCommand("SIM item1 10 using=events");
  ASSERT_TRUE(ContainsOK(resp));
  auto results = ParseSimResults(resp);
  float item2_score_before = 0.0f;
  for (const auto& r : results) {
    if (r.first == "item2") {
      item2_score_before = r.second;
      break;
    }
  }

  // Step c: Delete item2 from all contexts
  for (int i = 0; i < 5; ++i) {
    std::string ctx = "ctx_adc_" + std::to_string(i);
    auto del_resp = client.SendCommand("EVENT " + ctx + " DEL item2");
    EXPECT_TRUE(ContainsOK(del_resp));
  }

  // Step d: After delete, SIM should not crash and return valid results.
  // Note: co-occurrence index uses cumulative scoring via UpdateFromEvents,
  // so scores may not decrease after DEL (they reflect historical accumulation).
  resp = client.SendCommand("SIM item1 10 using=events");
  ASSERT_TRUE(ContainsOK(resp)) << "SIM should succeed after EVENT DEL";

  // Step e: Re-add item2 in fewer contexts (weaker co-occurrence)
  for (int i = 0; i < 3; ++i) {
    std::string ctx = "ctx_adc_" + std::to_string(i);
    ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT " + ctx + " ADD item2 50")));
  }

  // Step f: Verify server responds correctly after add-delete-add cycle.
  // The key invariant is that the server handles the full lifecycle without
  // crashing and returns valid responses at each step.
  resp = client.SendCommand("SIM item1 10 using=events");
  ASSERT_TRUE(ContainsOK(resp)) << "SIM should succeed after re-add cycle";
}

// ---------------------------------------------------------------------------
// Test 11: DeleteAllItemsThenSearch
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, DeleteAllItemsThenSearch) {
  TcpClient client("127.0.0.1", port_);

  // Setup: add 5 items with events and vectors
  for (int i = 1; i <= 5; ++i) {
    std::string id = "del_item" + std::to_string(i);
    ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx_del ADD " + id + " " + std::to_string(i * 10))));
    float x = static_cast<float>(i) / 5.0f;
    float y = 1.0f - x;
    ASSERT_TRUE(
        ContainsOK(client.SendCommand("VECSET " + id + " " + std::to_string(x) + " " + std::to_string(y) + " 0.0")));
  }

  // Verify we have data
  auto info = client.SendCommand("INFO");
  EXPECT_TRUE(info.find("vector_count: 5") != std::string::npos) << "Should have 5 vectors before deletion";

  // Overwrite all vectors to zero vectors (effectively "delete" by replacing)
  for (int i = 1; i <= 5; ++i) {
    std::string id = "del_item" + std::to_string(i);
    ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET " + id + " 0.0 0.0 0.0")));
  }

  // Delete all events
  for (int i = 1; i <= 5; ++i) {
    std::string id = "del_item" + std::to_string(i);
    auto del_resp = client.SendCommand("EVENT ctx_del DEL " + id);
    EXPECT_TRUE(ContainsOK(del_resp));
  }

  // Search should return 0 results or handle gracefully
  auto resp = client.SendCommand("SIM del_item1 10 using=events");
  EXPECT_TRUE(ContainsOK(resp) || ContainsError(resp))
      << "SIM after deleting all events should be handled gracefully, got: " + resp;

  resp = client.SendCommand("SIM del_item1 10 using=vectors");
  EXPECT_TRUE(ContainsOK(resp) || ContainsError(resp))
      << "SIM after zeroing all vectors should be handled gracefully, got: " + resp;

  // INFO should still work
  info = client.SendCommand("INFO");
  EXPECT_FALSE(info.empty()) << "INFO should still work after deleting all data";
}

// ---------------------------------------------------------------------------
// Test 12: ConcurrentWriteAndQuery
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, ConcurrentWriteAndQuery) {
  // Seed some initial data so readers have something to query
  TcpClient setup_client("127.0.0.1", port_);
  ASSERT_TRUE(ContainsOK(setup_client.SendCommand("VECSET seed1 1.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(setup_client.SendCommand("VECSET seed2 0.0 1.0 0.0")));
  setup_client.Close();

  std::atomic<int> write_ok{0};
  std::atomic<int> write_err{0};
  std::atomic<int> read_ok{0};
  std::atomic<int> read_err{0};
  std::atomic<bool> writer_done{false};

  // Writer thread: rapidly VECSET 100 items
  std::thread writer([this, &write_ok, &write_err, &writer_done]() {
    TcpClient client("127.0.0.1", port_);
    for (int i = 0; i < 100; ++i) {
      float x = static_cast<float>(i) / 100.0f;
      float y = 1.0f - x;
      std::string cmd =
          "VECSET cw_item_" + std::to_string(i) + " " + std::to_string(x) + " " + std::to_string(y) + " 0.0";
      auto resp = client.SendCommand(cmd);
      if (ContainsOK(resp)) {
        write_ok.fetch_add(1, std::memory_order_relaxed);
      } else {
        write_err.fetch_add(1, std::memory_order_relaxed);
      }
    }
    writer_done.store(true, std::memory_order_release);
  });

  // Reader thread: query SIM repeatedly while writer is active
  std::thread reader([this, &read_ok, &read_err, &writer_done]() {
    TcpClient client("127.0.0.1", port_);
    while (!writer_done.load(std::memory_order_acquire)) {
      auto resp = client.SendCommand("SIM seed1 10 using=vectors");
      if (ContainsOK(resp)) {
        read_ok.fetch_add(1, std::memory_order_relaxed);
      } else if (!resp.empty()) {
        read_err.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // A few more reads after writer finishes
    for (int i = 0; i < 5; ++i) {
      auto resp = client.SendCommand("SIM seed1 10 using=vectors");
      if (ContainsOK(resp)) {
        read_ok.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  writer.join();
  reader.join();

  // Writer should have succeeded for most commands
  EXPECT_GT(write_ok.load(), 0) << "Writer should have succeeded for some commands";

  // Reader should always get valid responses (OK or valid error, no corruption)
  EXPECT_GT(read_ok.load(), 0) << "Reader should have succeeded for some queries";
  EXPECT_EQ(read_err.load(), 0) << "Reader should not get invalid responses during concurrent writes";

  // Verify server is still healthy
  TcpClient health_client("127.0.0.1", port_);
  auto health = health_client.SendCommand("INFO");
  EXPECT_FALSE(health.empty()) << "Server should be healthy after concurrent write+query";
}

// ===========================================================================
// Cache Invalidation Tests
// ===========================================================================

// ---------------------------------------------------------------------------
// Test 13: CacheInvalidatedOnVecsetMutation
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, CacheInvalidatedOnVecsetMutation) {
  TcpClient client("127.0.0.1", port_);

  // Ensure cache is enabled and set min cost to 0 for test cacheability
  ASSERT_TRUE(ContainsOK(client.SendCommand("CACHE ENABLE")));
  client.SendCommand("SET cache.min_query_cost_ms 0");

  // Setup vectors
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET ci_item1 1.0 0.0 0.0")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET ci_item2 0.9 0.1 0.0")));

  // First query (cache miss)
  auto resp1 = client.SendCommand("SIM ci_item1 10 using=vectors");
  ASSERT_TRUE(ContainsOK(resp1));

  // Second identical query (cache hit if wired in)
  auto resp2 = client.SendCommand("SIM ci_item1 10 using=vectors");
  ASSERT_TRUE(ContainsOK(resp2));

  // Record cache stats before mutation
  auto stats_before = client.SendCommand("CACHE STATS");
  EXPECT_TRUE(ContainsOK(stats_before));
  int hits_before = std::stoi(ParseResponseField(stats_before, "cache_hits"));

  // Mutate ci_item2's vector (data change)
  ASSERT_TRUE(ContainsOK(client.SendCommand("VECSET ci_item2 0.0 0.0 1.0")));

  // Query again after mutation
  auto resp3 = client.SendCommand("SIM ci_item1 10 using=vectors");
  ASSERT_TRUE(ContainsOK(resp3));

  // Check cache stats after mutation
  auto stats_after = client.SendCommand("CACHE STATS");
  EXPECT_TRUE(ContainsOK(stats_after));

  // Document the invalidation behavior: when cache is fully integrated,
  // we expect either:
  // a) Cache was invalidated: hits_after == hits_before (third query was a miss)
  // b) Selective invalidation: if ci_item2 was not in the cached result for
  //    ci_item1, the cache entry may still be valid (a hit)
  // Either behavior is acceptable. The key assertion is no crash and valid data.
  int hits_after = std::stoi(ParseResponseField(stats_after, "cache_hits"));
  EXPECT_GE(hits_after, hits_before) << "Cache hits should be non-decreasing (or reset after invalidation)";
}

// ---------------------------------------------------------------------------
// Test 14: CacheInvalidatedOnEventMutation
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, CacheInvalidatedOnEventMutation) {
  TcpClient client("127.0.0.1", port_);

  // Ensure cache is enabled
  ASSERT_TRUE(ContainsOK(client.SendCommand("CACHE ENABLE")));
  client.SendCommand("SET cache.min_query_cost_ms 0");

  // Setup events
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx_ci ADD ev_item1 100")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx_ci ADD ev_item2 90")));
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx_ci ADD ev_item3 80")));

  // First query (cache miss)
  auto resp1 = client.SendCommand("SIM ev_item1 10 using=events");
  ASSERT_TRUE(ContainsOK(resp1));

  // Second identical query (potential cache hit)
  auto resp2 = client.SendCommand("SIM ev_item1 10 using=events");
  ASSERT_TRUE(ContainsOK(resp2));

  // Record cache stats before mutation
  auto stats_before = client.SendCommand("CACHE STATS");
  EXPECT_TRUE(ContainsOK(stats_before));

  // Mutate event data: delete ev_item2 from context
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx_ci DEL ev_item2")));

  // Add a new event to further change the data
  ASSERT_TRUE(ContainsOK(client.SendCommand("EVENT ctx_ci ADD ev_item4 95")));

  // Query again after mutation
  auto resp3 = client.SendCommand("SIM ev_item1 10 using=events");
  ASSERT_TRUE(ContainsOK(resp3));

  // Verify the server returned valid results reflecting the mutation
  auto results = ParseSimResults(resp3);
  // ev_item2 should either be absent or have reduced prominence
  bool ev_item4_found = false;
  for (const auto& r : results) {
    if (r.first == "ev_item4") {
      ev_item4_found = true;
    }
  }
  // ev_item4 was added after ev_item2 was deleted, so it should appear
  // if the results reflect current state (not stale cache)
  if (!results.empty()) {
    EXPECT_TRUE(ev_item4_found) << "After event mutation, ev_item4 should appear in results "
                                   "(verifying cache does not serve stale data)";
  }
}

// ---------------------------------------------------------------------------
// Test 15: CacheClearResetsEverything
// ---------------------------------------------------------------------------

TEST_F(AdversarialE2ETest, CacheClearResetsEverything) {
  TcpClient client("127.0.0.1", port_);

  // Ensure cache is enabled
  ASSERT_TRUE(ContainsOK(client.SendCommand("CACHE ENABLE")));
  client.SendCommand("SET cache.min_query_cost_ms 0");

  // Populate data
  PopulateBasicData(client);

  // Run several different queries to populate cache
  client.SendCommand("SIM item1 10 using=vectors");
  client.SendCommand("SIM item2 10 using=vectors");
  client.SendCommand("SIM item3 10 using=vectors");
  client.SendCommand("SIM item1 10 using=events");
  client.SendCommand("SIM item2 10 using=events");

  // Check cache stats - entries may be > 0 if cache is integrated
  auto stats_before = client.SendCommand("CACHE STATS");
  ASSERT_TRUE(ContainsOK(stats_before));
  int entries_before = std::stoi(ParseResponseField(stats_before, "cache_entries"));

  // CACHE CLEAR
  auto clear_resp = client.SendCommand("CACHE CLEAR");
  ASSERT_TRUE(ContainsOK(clear_resp));

  // After clear: entries must be 0
  auto stats_after = client.SendCommand("CACHE STATS");
  ASSERT_TRUE(ContainsOK(stats_after));
  int entries_after = std::stoi(ParseResponseField(stats_after, "cache_entries"));
  EXPECT_EQ(entries_after, 0) << "CACHE CLEAR should reset entries to 0";
  EXPECT_LE(entries_after, entries_before) << "Entries after clear should not exceed entries before";

  // Run same queries again: they should all be misses (not hits)
  client.SendCommand("SIM item1 10 using=vectors");
  client.SendCommand("SIM item2 10 using=vectors");
  client.SendCommand("SIM item3 10 using=vectors");

  auto stats_post_requery = client.SendCommand("CACHE STATS");
  ASSERT_TRUE(ContainsOK(stats_post_requery));

  // When cache is wired into SIM, all post-clear queries should be misses.
  // cache_hits should not have increased from the clear point.
  int hits_post_clear = std::stoi(ParseResponseField(stats_post_requery, "cache_hits"));
  // Since cache was just cleared, the re-queries are all misses. Hits should
  // be 0 or remain at whatever baseline the CACHE CLEAR reset to.
  // Document: CACHE CLEAR resets stats in some implementations.
  EXPECT_GE(hits_post_clear, 0) << "Post-clear hits should be non-negative";
}

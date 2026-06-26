/**
 * @file wal_recovery_integration_test.cpp
 * @brief Integration tests for Write-Ahead Log crash recovery
 *
 * Proves the end-to-end durability contract wired into the server:
 * - Live writes through the dispatcher append to the WAL.
 * - A fresh set of stores recovers identical state by replaying the WAL.
 * - The snapshot + WAL boundary is exact: after a DUMP SAVE records a checkpoint
 *   and truncates the WAL, recovery loads the snapshot then replays only the
 *   records beyond the checkpoint, so co-occurrence scores are NOT double
 *   counted.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

#include "cache/similarity_cache.h"
#include "config/config.h"
#include "config/runtime_variable_manager.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/request_dispatcher.h"
#include "server/server_types.h"
#include "similarity/similarity_engine.h"
#include "storage/snapshot_format_v1.h"
#include "storage/snapshot_lock.h"
#include "storage/wal.h"
#include "storage/wal_checkpoint.h"
#include "vectors/metadata_store.h"
#include "vectors/vector_store.h"

namespace fs = std::filesystem;

using namespace nvecd;
using namespace nvecd::server;

namespace {

/**
 * @brief A self-contained server-like instance (stores + dispatcher + WAL).
 *
 * Mirrors the wiring NvecdServer performs in InitializeComponents so the
 * recovery ordering (Open -> Replay with wal=null -> publish wal) can be
 * exercised directly without a TCP server.
 */
struct Instance {
  ServerStats stats;
  std::atomic<bool> loading{false};
  std::atomic<bool> read_only{false};
  std::unique_ptr<config::Config> config;
  std::unique_ptr<events::EventStore> event_store;
  std::unique_ptr<events::CoOccurrenceIndex> co_index;
  std::unique_ptr<vectors::VectorStore> vector_store;
  std::unique_ptr<vectors::MetadataStore> metadata_store;
  std::unique_ptr<similarity::SimilarityEngine> similarity_engine;
  std::unique_ptr<cache::SimilarityCache> cache;
  std::unique_ptr<config::RuntimeVariableManager> variable_manager;
  storage::WriteAheadLog wal;

  alignas(HandlerContext) char ctx_storage[sizeof(HandlerContext)]{};  // NOLINT(modernize-avoid-c-arrays)
  HandlerContext* ctx = nullptr;
  std::unique_ptr<RequestDispatcher> dispatcher;

  explicit Instance(const std::string& dump_dir) {
    config = std::make_unique<config::Config>();
    config->snapshot.mode = "lock";  // synchronous save so the test is deterministic

    config::EventsConfig events_cfg;
    event_store = std::make_unique<events::EventStore>(events_cfg);
    co_index = std::make_unique<events::CoOccurrenceIndex>();

    config::VectorsConfig vectors_cfg;
    vector_store = std::make_unique<vectors::VectorStore>(vectors_cfg);
    metadata_store = std::make_unique<vectors::MetadataStore>();

    config::SimilarityConfig sim_cfg;
    similarity_engine = std::make_unique<similarity::SimilarityEngine>(
        event_store.get(), co_index.get(), vector_store.get(), sim_cfg, vectors_cfg, metadata_store.get());

    cache = std::make_unique<cache::SimilarityCache>(1024 * 1024, 0, 0);

    auto manager_result = config::RuntimeVariableManager::Create(*config);
    variable_manager = std::move(*manager_result);

    ctx = new (ctx_storage) HandlerContext{/*.event_store=*/event_store.get(),
                                           /*.co_index=*/co_index.get(),
                                           /*.vector_store=*/vector_store.get(),
                                           /*.metadata_store=*/metadata_store.get(),
                                           /*.similarity_engine=*/similarity_engine.get(),
                                           /*.cache=*/{},
                                           /*.variable_manager=*/variable_manager.get(),
                                           /*.stats=*/stats,
                                           /*.config=*/config.get(),
                                           /*.loading=*/loading,
                                           /*.read_only=*/read_only,
                                           /*.dump_dir=*/dump_dir,
                                           /*.requirepass=*/""};
    ctx->cache.store(cache.get(), std::memory_order_release);

    dispatcher = std::make_unique<RequestDispatcher>(*ctx);
  }

  ~Instance() {
    dispatcher.reset();
    if (wal.IsOpen()) {
      wal.Close();
    }
    if (ctx != nullptr) {
      ctx->~HandlerContext();
      ctx = nullptr;
    }
  }

  /// Open the WAL in @p wal_dir (publishes nothing; replay happens separately).
  void OpenWal(const std::string& wal_dir) {
    storage::WriteAheadLog::Config wal_config;
    wal_config.directory = wal_dir;
    wal_config.sync_on_write = true;  // deterministic durability for the test
    auto opened = wal.Open(wal_config);
    ASSERT_TRUE(opened.has_value()) << opened.error().message();
  }

  /// Publish the WAL so live dispatch appends records.
  void EnableWalForLiveWrites() { ctx->wal = &wal; }

  /// Replay records >= @p from while the WAL is NOT published (no re-append).
  uint64_t Replay(uint64_t from) {
    ctx->wal = nullptr;  // ensure no re-append during replay
    auto replayed = wal.Replay(from, [this](const storage::WalRecord& r) { dispatcher->ReplayRecord(r); });
    EXPECT_TRUE(replayed.has_value()) << replayed.error().message();
    return replayed ? *replayed : 0;
  }

  std::string Dispatch(const std::string& request) {
    ConnectionContext conn_ctx;
    conn_ctx.authenticated = true;
    return dispatcher->Dispatch(request + "\r\n", conn_ctx);
  }
};

/// Create a unique temporary working directory for a test case.
std::string MakeTempDir(const std::string& tag) {
  auto base = fs::temp_directory_path() /
              ("nvecd_wal_recovery_" + tag + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
               "_" + std::to_string(reinterpret_cast<uintptr_t>(&tag)));
  fs::create_directories(base);
  return base.string();
}

}  // namespace

// ============================================================================
// Full WAL replay (no snapshot): a fresh instance reconstructs identical state.
// ============================================================================

TEST(WalRecoveryIntegration, RecoversStateFromWalReplay) {
  const std::string root = MakeTempDir("replay");
  const std::string wal_dir = root + "/wal";
  fs::create_directories(wal_dir);

  // --- Original instance: ingest with the WAL wired for live writes. ---
  {
    Instance a(root);
    a.OpenWal(wal_dir);
    a.EnableWalForLiveWrites();

    // Co-occurrence: item_a and item_b share context ctx1.
    ASSERT_NE(a.Dispatch("EVENT ctx1 ADD item_a 90").find("OK"), std::string::npos);
    ASSERT_NE(a.Dispatch("EVENT ctx1 ADD item_b 80").find("OK"), std::string::npos);

    // Vectors + metadata.
    ASSERT_NE(a.Dispatch("VECSET item_a 1 0").find("OK"), std::string::npos);
    ASSERT_NE(a.Dispatch("VECSET item_b 0 1").find("OK"), std::string::npos);
    ASSERT_NE(a.Dispatch("METASET item_a status:active,rank:10").find("OK METASET"), std::string::npos);

    // Capture the co-occurrence score that recovery must reproduce exactly.
    const float original_score = a.co_index->GetScore("item_a", "item_b");
    EXPECT_GT(original_score, 0.0F);
    a.wal.Close();

    // --- Recovery instance: fresh stores, replay the entire WAL. ---
    Instance b(root);
    b.OpenWal(wal_dir);
    const uint64_t replayed = b.Replay(/*from=*/0);
    EXPECT_GE(replayed, 5U);  // 2 events + 2 vectors + 1 metaset
    b.EnableWalForLiveWrites();

    // Vectors recovered.
    EXPECT_TRUE(b.vector_store->HasVector("item_a"));
    EXPECT_TRUE(b.vector_store->HasVector("item_b"));

    // Metadata recovered (verbatim filter_expr re-parsed).
    const auto* meta = b.metadata_store->Get("item_a");
    ASSERT_NE(meta, nullptr);
    EXPECT_NE(meta->find("status"), meta->end());

    // Co-occurrence score recovered identically.
    EXPECT_FLOAT_EQ(b.co_index->GetScore("item_a", "item_b"), original_score);
  }

  fs::remove_all(root);
}

// ============================================================================
// Snapshot + WAL boundary: pre- and post-snapshot ops present, no double-count.
// ============================================================================

TEST(WalRecoveryIntegration, SnapshotBoundaryNoDoubleCount) {
  const std::string root = MakeTempDir("boundary");
  const std::string wal_dir = root + "/wal";
  fs::create_directories(wal_dir);
  const std::string snapshot_path = root + "/snap.nvec";

  float pre_snapshot_score = 0.0F;
  {
    Instance a(root);
    a.OpenWal(wal_dir);
    a.EnableWalForLiveWrites();

    // Pre-snapshot co-occurrence between item_a and item_b.
    ASSERT_NE(a.Dispatch("EVENT ctx1 ADD item_a 90").find("OK"), std::string::npos);
    ASSERT_NE(a.Dispatch("EVENT ctx1 ADD item_b 80").find("OK"), std::string::npos);
    ASSERT_NE(a.Dispatch("VECSET item_a 1 0").find("OK"), std::string::npos);
    ASSERT_NE(a.Dispatch("VECSET item_b 0 1").find("OK"), std::string::npos);

    pre_snapshot_score = a.co_index->GetScore("item_a", "item_b");
    EXPECT_GT(pre_snapshot_score, 0.0F);

    // DUMP SAVE (lock mode) captures the WAL sequence under the write barrier,
    // writes the checkpoint sidecar, and truncates the WAL up to that sequence.
    uint64_t captured = 0;
    auto save = storage::WriteSnapshotWithLock(snapshot_path, *a.config, *a.event_store, *a.co_index, *a.vector_store,
                                               nullptr, nullptr, a.metadata_store.get(), &a.wal, &captured);
    ASSERT_TRUE(save.has_value()) << save.error().message();
    auto checkpoint = storage::WriteWalCheckpoint(snapshot_path, captured);
    ASSERT_TRUE(checkpoint.has_value()) << checkpoint.error().message();
    auto truncated = a.wal.Truncate(captured);
    ASSERT_TRUE(truncated.has_value()) << truncated.error().message();

    // Post-snapshot ops: a new co-occurring item and a new vector.
    ASSERT_NE(a.Dispatch("EVENT ctx2 ADD item_a 70").find("OK"), std::string::npos);
    ASSERT_NE(a.Dispatch("EVENT ctx2 ADD item_c 60").find("OK"), std::string::npos);
    ASSERT_NE(a.Dispatch("VECSET item_c 0 1").find("OK"), std::string::npos);

    a.wal.Close();
  }

  // --- Recovery: load snapshot, then replay WAL from checkpoint + 1. ---
  {
    Instance b(root);

    // Load the snapshot (pre-snapshot state).
    config::Config loaded_config;
    auto load =
        storage::snapshot_v1::ReadSnapshotV1(snapshot_path, loaded_config, *b.event_store, *b.co_index, *b.vector_store,
                                             nullptr, nullptr, nullptr, b.metadata_store.get());
    ASSERT_TRUE(load.has_value()) << load.error().message();

    // Pre-snapshot score is present from the snapshot alone.
    EXPECT_FLOAT_EQ(b.co_index->GetScore("item_a", "item_b"), pre_snapshot_score);

    // Replay only records beyond the checkpoint.
    b.OpenWal(wal_dir);
    const uint64_t checkpoint = storage::ReadWalCheckpoint(snapshot_path);
    EXPECT_GT(checkpoint, 0U);
    b.Replay(/*from=*/checkpoint + 1);
    b.EnableWalForLiveWrites();

    // Post-snapshot ops are present.
    EXPECT_TRUE(b.vector_store->HasVector("item_c"));
    EXPECT_GT(b.co_index->GetScore("item_a", "item_c"), 0.0F);

    // CRITICAL: the pre-snapshot co-occurrence score is NOT double counted.
    // The snapshot contributed it once; replaying from checkpoint+1 must skip
    // the events that the snapshot already absorbed.
    EXPECT_FLOAT_EQ(b.co_index->GetScore("item_a", "item_b"), pre_snapshot_score);
  }

  fs::remove_all(root);
}

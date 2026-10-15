/**
 * @file command_stats_test.cpp
 * @brief Per-group command accounting and its TCP INFO report
 */

#include <gtest/gtest.h>

#include <atomic>
#include <numeric>
#include <string>

#include "config/config.h"
#include "server/handlers/info_handler.h"
#include "server/server_types.h"
#include "vectors/vector_store.h"

namespace nvecd::server {
namespace {

uint64_t SumCalls(const ServerStats& stats) {
  uint64_t sum = 0;
  for (const auto& stat : stats.CommandGroupStats()) {
    sum += stat.calls;
  }
  return sum;
}

TEST(CommandStatsTest, EveryCommandTypeLandsInExactlyOneGroup) {
  ServerStats stats;
  for (size_t i = 0; i < static_cast<size_t>(CommandType::kCount); ++i) {
    CommandStatsScope scope(stats, static_cast<CommandType>(i));
    scope.MarkSucceeded();
  }
  EXPECT_EQ(SumCalls(stats), stats.total_commands.load());
  EXPECT_EQ(stats.failed_commands.load(), 0U);
}

TEST(CommandStatsTest, FailuresAreAttributedToTheCommandGroup) {
  ServerStats stats;
  {
    CommandStatsScope ok(stats, CommandType::kVecdel);
    ok.MarkSucceeded();
  }
  { CommandStatsScope failed(stats, CommandType::kVecdel); }
  { CommandStatsScope failed(stats, CommandType::kDumpLoad); }

  const auto groups = stats.CommandGroupStats();
  const auto& vecdel = groups[static_cast<size_t>(CommandGroup::kVecdel)];
  EXPECT_EQ(vecdel.calls, 2U);
  EXPECT_EQ(vecdel.failed, 1U);
  const auto& dump = groups[static_cast<size_t>(CommandGroup::kDump)];
  EXPECT_EQ(dump.calls, 1U);
  EXPECT_EQ(dump.failed, 1U);

  uint64_t failed_sum = 0;
  for (const auto& stat : groups) {
    failed_sum += stat.failed;
  }
  EXPECT_EQ(failed_sum, stats.failed_commands.load());
}

TEST(CommandStatsTest, InfoReportsEveryGroupAndWalReplayGap) {
  ServerStats stats;
  std::atomic<bool> loading{false};
  std::atomic<bool> read_only{false};
  HandlerContext ctx{.stats = stats, .loading = loading, .read_only = read_only};
  { CommandStatsScope failed(stats, CommandType::kMetaset); }
  stats.wal_replay_records_skipped.store(7);

  auto info = handlers::HandleInfo(ctx);
  ASSERT_TRUE(info.has_value());
  EXPECT_NE(info->find("# Commandstats"), std::string::npos);
  for (size_t i = 0; i < kCommandGroupCount; ++i) {
    const std::string key = "cmdstat_" + std::string(CommandGroupName(static_cast<CommandGroup>(i))) + ": calls=";
    EXPECT_NE(info->find(key), std::string::npos) << key;
  }
  EXPECT_NE(info->find("cmdstat_metaset: calls=1,failed=1"), std::string::npos);
  EXPECT_NE(info->find("wal_replay_records_skipped: 7"), std::string::npos);
}

TEST(CommandStatsTest, UsedMemoryBytesCountsTheVectorMatrixOnly) {
  ServerStats stats;
  std::atomic<bool> loading{false};
  std::atomic<bool> read_only{false};
  config::VectorsConfig vectors_config;
  vectors_config.default_dimension = 4;
  vectors::VectorStore store(vectors_config);
  ASSERT_TRUE(store.SetVector("a", {1.0F, 0.0F, 0.0F, 0.0F}).has_value());
  ASSERT_TRUE(store.SetVector("b", {0.0F, 1.0F, 0.0F, 0.0F}).has_value());
  HandlerContext ctx{.vector_store = &store, .stats = stats, .loading = loading, .read_only = read_only};

  const auto figures = handlers::GatherMemoryFigures(ctx);
  EXPECT_EQ(figures.vector_matrix_bytes, 2U * 4U * sizeof(float));
  EXPECT_GE(figures.vectors_bytes, figures.vector_matrix_bytes);
  EXPECT_EQ(figures.StoresTotal(), figures.events_bytes + figures.vectors_bytes + figures.co_index_bytes);

  auto info = handlers::HandleInfo(ctx);
  ASSERT_TRUE(info.has_value());
  EXPECT_NE(info->find("used_memory_bytes: 32\n"), std::string::npos);
}

}  // namespace
}  // namespace nvecd::server

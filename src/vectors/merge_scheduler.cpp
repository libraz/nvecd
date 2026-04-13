/**
 * @file merge_scheduler.cpp
 * @brief Background merge/rebuild scheduler implementation
 */

#include "vectors/merge_scheduler.h"

#include "utils/structured_log.h"
#include "vectors/tiered_vector_store.h"

namespace nvecd::vectors {

MergeScheduler::MergeScheduler() = default;

MergeScheduler::MergeScheduler(const Config& config) : config_(config) {}

MergeScheduler::~MergeScheduler() { Stop(); }

void MergeScheduler::Start(TieredVectorStore* store) {
  if (running_.load(std::memory_order_acquire)) {
    return;
  }

  store_ = store;
  running_.store(true, std::memory_order_release);
  worker_ = std::thread(&MergeScheduler::CheckLoop, this);
}

void MergeScheduler::Stop() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  running_.store(false, std::memory_order_release);
  {
    std::lock_guard lock(cv_mutex_);
    cv_.notify_all();
  }

  if (worker_.joinable()) {
    worker_.join();
  }
}

void MergeScheduler::CheckLoop() {
  while (running_.load(std::memory_order_acquire)) {
    // Wait for check_interval or until stopped
    {
      std::unique_lock lock(cv_mutex_);
      cv_.wait_for(lock, config_.check_interval, [this] {
        return !running_.load(std::memory_order_acquire);
      });
    }

    if (!running_.load(std::memory_order_acquire)) {
      break;
    }

    // Check and perform merge if needed
    if (store_->NeedsMerge()) {
      utils::StructuredLog()
          .Event("merge_triggered")
          .Field("component", "merge_scheduler")
          .Field("delta_size", static_cast<uint64_t>(store_->DeltaSize()))
          .Info();
      auto result = store_->MergeDeltaToMain();
      if (!result) {
        utils::StructuredLog()
            .Event("merge_failed")
            .Field("component", "merge_scheduler")
            .Field("error", result.error().message())
            .Error();
      }
    }

    // Check and perform rebuild if needed
    if (store_->NeedsRebuild()) {
      utils::StructuredLog()
          .Event("rebuild_triggered")
          .Field("component", "merge_scheduler")
          .Field("deleted_count",
                 static_cast<uint64_t>(store_->DeletedCount()))
          .Info();
      auto result = store_->RebuildMain();
      if (!result) {
        utils::StructuredLog()
            .Event("rebuild_failed")
            .Field("component", "merge_scheduler")
            .Field("error", result.error().message())
            .Error();
      }
    }
  }
}

}  // namespace nvecd::vectors

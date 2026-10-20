/**
 * @file ivf_index_training_concurrency_test.cpp
 * @brief IVF training must not block searches or writes, and must not lose concurrent changes
 */

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#include "vectors/distance.h"
#include "vectors/ivf_index.h"

namespace nvecd::vectors {
namespace {

constexpr uint32_t kDim = 8;
constexpr size_t kTrained = 200;
constexpr size_t kRows = kTrained + 1;  // One extra row that is never in the training set
constexpr auto kTimeout = std::chrono::seconds(10);

class IvfTrainingConcurrencyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::mt19937 rng(7);
    std::normal_distribution<float> dist(0.0F, 1.0F);
    matrix_.resize(kRows * kDim);
    for (auto& v : matrix_) {
      v = dist(rng);
    }
    norms_.resize(kRows);
    for (size_t i = 0; i < kRows; ++i) {
      norms_[i] = L2Norm(Row(i));
    }
    valid_.resize(kTrained);
    std::iota(valid_.begin(), valid_.end(), size_t{0});

    IvfIndex::Config config;
    config.nlist = 4;
    config.nprobe = 4;  // Probe every cluster so results are exhaustive
    index_ = std::make_unique<IvfIndex>(kDim, config);
  }

  std::vector<float> Row(size_t i) const {
    return {matrix_.begin() + static_cast<ptrdiff_t>(i * kDim),
            matrix_.begin() + static_cast<ptrdiff_t>((i + 1) * kDim)};
  }

  std::vector<std::pair<float, size_t>> SearchRow(size_t i, size_t top_k) const {
    const auto query = Row(i);
    return index_->Search(query.data(), norms_[i], matrix_.data(), norms_.data(), kRows, kDim, top_k);
  }

  void TrainAssigned() { index_->Train(matrix_.data(), valid_.data(), valid_.size(), kDim, true); }

  /// The production training call: centroids only, rows reach the lists through the buffer.
  void TrainUnassigned() { index_->Train(matrix_.data(), valid_.data(), valid_.size(), kDim, false); }

  /// Holds a training run inside its lock-free phase until released.
  struct Gate {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> release_future{release.get_future().share()};
  };

  std::vector<float> matrix_;
  std::vector<float> norms_;
  std::vector<size_t> valid_;
  std::unique_ptr<IvfIndex> index_;
};

TEST_F(IvfTrainingConcurrencyTest, SearchAndWritesCompleteWhileTrainingRuns) {
  Gate gate;
  index_->SetTrainHook([&gate] {
    gate.entered.set_value();
    gate.release_future.wait();
  });

  std::thread trainer([this] { TrainAssigned(); });
  ASSERT_EQ(gate.entered.get_future().wait_for(kTimeout), std::future_status::ready);

  const auto row = Row(0);
  auto search = std::async(std::launch::async, [this] { return SearchRow(0, 3); });
  auto append = std::async(std::launch::async, [this, &row] { index_->AppendToBuffer(kTrained, row.data()); });
  auto state = std::async(std::launch::async, [this] {
    index_->SetNprobe(2);
    return index_->IsTrained() || index_->GetIndexedCount() == 0;
  });

  const bool search_done = search.wait_for(kTimeout) == std::future_status::ready;
  const bool append_done = append.wait_for(kTimeout) == std::future_status::ready;
  const bool state_done = state.wait_for(kTimeout) == std::future_status::ready;

  gate.release.set_value();
  trainer.join();

  EXPECT_TRUE(search_done) << "search blocked behind training";
  EXPECT_TRUE(append_done) << "buffer write blocked behind training";
  EXPECT_TRUE(state_done) << "state accessors blocked behind training";
  EXPECT_TRUE(index_->IsTrained());
}

TEST_F(IvfTrainingConcurrencyTest, VectorAddedDuringRetrainingIsIndexed) {
  TrainAssigned();

  Gate gate;
  index_->SetTrainHook([&gate] {
    gate.entered.set_value();
    gate.release_future.wait();
  });
  std::thread trainer([this] { TrainAssigned(); });
  ASSERT_EQ(gate.entered.get_future().wait_for(kTimeout), std::future_status::ready);

  const auto row = Row(kTrained);
  index_->AddVector(kTrained, row.data());

  gate.release.set_value();
  trainer.join();

  ASSERT_TRUE(index_->IsTrained());
  EXPECT_EQ(index_->GetIndexedCount(), kRows);
  const auto results = SearchRow(kTrained, 1);
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].second, kTrained);
}

TEST_F(IvfTrainingConcurrencyTest, VectorAddedDuringFirstTrainingOfAnUnsizedIndexIsIndexed) {
  IvfIndex::Config config;
  config.nlist = 4;
  config.nprobe = 4;
  index_ = std::make_unique<IvfIndex>(0, config);

  Gate gate;
  index_->SetTrainHook([&gate] {
    gate.entered.set_value();
    gate.release_future.wait();
  });
  std::thread trainer([this] { TrainAssigned(); });
  ASSERT_EQ(gate.entered.get_future().wait_for(kTimeout), std::future_status::ready);

  const auto row = Row(kTrained);
  index_->AddVector(kTrained, row.data());

  gate.release.set_value();
  trainer.join();

  ASSERT_TRUE(index_->IsTrained());
  EXPECT_EQ(index_->GetIndexedCount(), kRows);
}

TEST_F(IvfTrainingConcurrencyTest, VectorRemovedDuringRetrainingStaysRemoved) {
  TrainAssigned();

  Gate gate;
  index_->SetTrainHook([&gate] {
    gate.entered.set_value();
    gate.release_future.wait();
  });
  std::thread trainer([this] { TrainAssigned(); });
  ASSERT_EQ(gate.entered.get_future().wait_for(kTimeout), std::future_status::ready);

  index_->RemoveVector(5);

  gate.release.set_value();
  trainer.join();

  ASSERT_TRUE(index_->IsTrained());
  EXPECT_EQ(index_->GetIndexedCount(), kTrained - 1);
  for (const auto& [score, idx] : SearchRow(5, kRows)) {
    EXPECT_NE(idx, 5U);
  }
}

TEST_F(IvfTrainingConcurrencyTest, BufferedRowsWrittenAndRemovedDuringTrainingAreKeptOrDropped) {
  Gate gate;
  index_->SetTrainHook([&gate] {
    gate.entered.set_value();
    gate.release_future.wait();
  });
  std::thread trainer([this] { TrainUnassigned(); });
  ASSERT_EQ(gate.entered.get_future().wait_for(kTimeout), std::future_status::ready);

  const auto kept = Row(kTrained);
  const auto dropped = Row(1);
  index_->AppendToBuffer(kTrained, kept.data());
  index_->AppendToBuffer(1, dropped.data());
  index_->RemoveVector(1);

  gate.release.set_value();
  trainer.join();

  ASSERT_TRUE(index_->IsTrained());
  EXPECT_EQ(index_->GetIndexedCount(), 0U);
  EXPECT_EQ(index_->GetBufferSize(), 1U);
  const auto results = SearchRow(kTrained, kRows);
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].second, kTrained);
}

TEST_F(IvfTrainingConcurrencyTest, SealDuringRetrainingKeepsTheSealedRows) {
  TrainAssigned();
  const auto row = Row(kTrained);
  index_->AppendToBuffer(kTrained, row.data());

  Gate gate;
  index_->SetTrainHook([&gate] {
    gate.entered.set_value();
    gate.release_future.wait();
  });
  std::thread trainer([this] { TrainAssigned(); });
  ASSERT_EQ(gate.entered.get_future().wait_for(kTimeout), std::future_status::ready);

  // The seal publishes into the old lists, which the retrain is about to replace.
  EXPECT_TRUE(index_->SealBuffer());

  gate.release.set_value();
  trainer.join();

  ASSERT_TRUE(index_->IsTrained());
  EXPECT_EQ(index_->GetIndexedCount() + index_->GetBufferSize(), kRows);
  const auto results = SearchRow(kTrained, 1);
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].second, kTrained);
}

TEST_F(IvfTrainingConcurrencyTest, ResetDuringTrainingDiscardsTheResult) {
  Gate gate;
  index_->SetTrainHook([&gate] {
    gate.entered.set_value();
    gate.release_future.wait();
  });
  std::thread trainer([this] { TrainAssigned(); });
  ASSERT_EQ(gate.entered.get_future().wait_for(kTimeout), std::future_status::ready);

  index_->Reset(kDim);

  gate.release.set_value();
  trainer.join();

  EXPECT_FALSE(index_->IsTrained());
  EXPECT_EQ(index_->GetIndexedCount(), 0U);
}

TEST_F(IvfTrainingConcurrencyTest, QuiescentTrainingAssignsEveryVector) {
  TrainAssigned();

  EXPECT_TRUE(index_->IsTrained());
  EXPECT_EQ(index_->GetClusterCount(), 4U);
  EXPECT_EQ(index_->GetIndexedCount(), kTrained);
  const auto results = SearchRow(3, 1);
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].second, 3U);
}

}  // namespace
}  // namespace nvecd::vectors

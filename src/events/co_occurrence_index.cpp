/**
 * @file co_occurrence_index.cpp
 * @brief Co-occurrence index implementation
 */

#include "events/co_occurrence_index.h"

#include <algorithm>
#include <cmath>

namespace nvecd::events {

void CoOccurrenceIndex::UpdateFromEvents(
    const std::string& ctx [[maybe_unused]],
    const std::vector<Event>& events) {
  if (events.empty()) {
    return;
  }

  // Compute pairwise co-occurrence scores
  std::unique_lock lock(mutex_);

  for (size_t i = 0; i < events.size(); ++i) {
    for (size_t j = i + 1; j < events.size(); ++j) {
      const auto& event1 = events[i];
      const auto& event2 = events[j];

      if (event1.id == event2.id) {
        continue;  // Skip self-pairs
      }

      // Calculate co-occurrence score: score1 * score2
      float score = static_cast<float>(event1.score * event2.score);

      // Store symmetric scores (both directions)
      co_scores_[event1.id][event2.id] += score;
      co_scores_[event2.id][event1.id] += score;
    }
  }
}

std::vector<std::pair<std::string, float>> CoOccurrenceIndex::GetSimilar(
    const std::string& id, int top_k) const {
  if (top_k <= 0) {
    return {};
  }

  std::shared_lock lock(mutex_);

  auto it = co_scores_.find(id);
  if (it == co_scores_.end()) {
    return {};
  }

  // Collect all co-occurring items with scores
  std::vector<std::pair<std::string, float>> results;
  results.reserve(it->second.size());

  for (const auto& [other_id, score] : it->second) {
    if (score > 0.0f) {  // Only include positive scores
      results.emplace_back(other_id, score);
    }
  }

  // Sort by score descending
  std::sort(results.begin(), results.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  // Return top-k results
  if (results.size() > static_cast<size_t>(top_k)) {
    results.resize(static_cast<size_t>(top_k));
  }

  return results;
}

float CoOccurrenceIndex::GetScore(const std::string& id1,
                                  const std::string& id2) const {
  std::shared_lock lock(mutex_);

  auto it1 = co_scores_.find(id1);
  if (it1 == co_scores_.end()) {
    return 0.0f;
  }

  auto it2 = it1->second.find(id2);
  if (it2 == it1->second.end()) {
    return 0.0f;
  }

  return it2->second;
}

void CoOccurrenceIndex::ApplyDecay(double alpha) {
  if (alpha <= 0.0 || alpha > 1.0) {
    return;  // Invalid alpha, skip decay
  }

  std::unique_lock lock(mutex_);

  for (auto& [id1, scores_map] : co_scores_) {
    for (auto& [id2, score] : scores_map) {
      score *= static_cast<float>(alpha);
    }
  }
}

size_t CoOccurrenceIndex::GetItemCount() const {
  std::shared_lock lock(mutex_);
  return co_scores_.size();
}

std::vector<std::string> CoOccurrenceIndex::GetAllItems() const {
  std::shared_lock lock(mutex_);

  std::vector<std::string> items;
  items.reserve(co_scores_.size());

  for (const auto& [id, _] : co_scores_) {
    items.push_back(id);
  }

  return items;
}

void CoOccurrenceIndex::Clear() {
  std::unique_lock lock(mutex_);
  co_scores_.clear();
}

CoOccurrenceIndexStatistics CoOccurrenceIndex::GetStatistics() const {
  std::shared_lock lock(mutex_);

  CoOccurrenceIndexStatistics stats;
  stats.tracked_ids = co_scores_.size();

  // Count co-occurrence pairs
  size_t pairs = 0;
  for (const auto& [id1, neighbors] : co_scores_) {
    pairs += neighbors.size();
  }
  stats.co_pairs = pairs / 2;  // Symmetric matrix, so divide by 2

  stats.memory_bytes = MemoryUsage();

  return stats;
}

size_t CoOccurrenceIndex::MemoryUsage() const {
  std::shared_lock lock(mutex_);

  size_t total = 0;

  // Base container overhead
  total += sizeof(*this);

  // Co-occurrence matrix
  for (const auto& [id1, neighbors] : co_scores_) {
    // Outer map key (id1)
    total += sizeof(std::string) + id1.capacity();

    // Inner map
    total += sizeof(std::unordered_map<std::string, float>);
    for (const auto& [id2, score] : neighbors) {
      total += sizeof(std::string) + id2.capacity();
      total += sizeof(float);
    }
  }

  return total;
}

}  // namespace nvecd::events

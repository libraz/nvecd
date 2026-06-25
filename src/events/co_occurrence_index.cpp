/**
 * @file co_occurrence_index.cpp
 * @brief Co-occurrence index implementation
 */

#include "events/co_occurrence_index.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace nvecd::events {

CoOccurrenceIndex::CoOccurrenceIndex(const Config& config) : config_(config) {}

void CoOccurrenceIndex::UpdateFromEvents(const std::string& ctx, const std::vector<Event>& events) {
  if (events.empty()) {
    return;
  }
  std::unique_lock lock(mutex_);
  UpdateFromEventsInternal(ctx, events, false, 0.0);
}

void CoOccurrenceIndex::UpdateFromEvents(const std::string& ctx, const std::vector<Event>& events,
                                         bool temporal_enabled, double half_life_sec) {
  if (events.empty()) {
    return;
  }
  std::unique_lock lock(mutex_);
  UpdateFromEventsInternal(ctx, events, temporal_enabled, half_life_sec);
}

void CoOccurrenceIndex::UpdateFromEventsLocked(const std::string& ctx, const std::vector<Event>& events,
                                               bool temporal_enabled, double half_life_sec) {
  if (events.empty()) {
    return;
  }
  // Caller holds the lock
  UpdateFromEventsInternal(ctx, events, temporal_enabled, half_life_sec);
}

void CoOccurrenceIndex::ApplyIngestedEvent(const std::string& ctx [[maybe_unused]],
                                           const std::vector<Event>& prior_events, const Event& new_event,
                                           const IngestOptions& options) {
  std::unique_lock lock(mutex_);

  // Incrementally score the new event against the prior buffer (once each).
  AddEventIncrementalInternal(prior_events, new_event, options.temporal_enabled, options.half_life_sec);

  // For deletions with negative signals enabled, dampen the removed item's
  // associations with the items it previously co-occurred with.
  if (options.negative_signals && new_event.type == EventType::DEL) {
    ApplyNegativeSignalLocked(new_event.item_id, prior_events, options.negative_weight);
  }
}

void CoOccurrenceIndex::AddEventIncremental(const std::string& ctx [[maybe_unused]],
                                            const std::vector<Event>& prior_events, const Event& new_event,
                                            bool temporal_enabled, double half_life_sec) {
  std::unique_lock lock(mutex_);
  AddEventIncrementalInternal(prior_events, new_event, temporal_enabled, half_life_sec);
}

void CoOccurrenceIndex::AddEventIncrementalLocked(const std::string& ctx [[maybe_unused]],
                                                  const std::vector<Event>& prior_events, const Event& new_event,
                                                  bool temporal_enabled, double half_life_sec) {
  // Caller holds the lock
  AddEventIncrementalInternal(prior_events, new_event, temporal_enabled, half_life_sec);
}

void CoOccurrenceIndex::AddEventIncrementalInternal(const std::vector<Event>& prior_events, const Event& new_event,
                                                    bool temporal_enabled, double half_life_sec) {
  if (prior_events.empty()) {
    return;
  }

  // Pre-compute the decay weight for the new event and each prior event.
  // Ages are measured relative to the most recent timestamp across all
  // events involved (new + prior), mirroring the full-scan path so that the
  // incremental result matches a from-scratch computation.
  float new_decay = 1.0F;
  std::vector<float> prior_decay;
  if (temporal_enabled && half_life_sec > 0.0) {
    uint64_t max_ts = new_event.timestamp;
    for (const auto& e : prior_events) {
      if (e.timestamp > max_ts) {
        max_ts = e.timestamp;
      }
    }

    const double inv_half_life = 1.0 / half_life_sec;
    constexpr float kMinDecay = 1e-6F;
    auto decay_for = [&](uint64_t ts) {
      double age = static_cast<double>(max_ts - ts);
      auto decay = static_cast<float>(std::exp2(-age * inv_half_life));
      return std::max(decay, kMinDecay);
    };

    new_decay = decay_for(new_event.timestamp);
    prior_decay.resize(prior_events.size());
    for (size_t i = 0; i < prior_events.size(); ++i) {
      prior_decay[i] = decay_for(prior_events[i].timestamp);
    }
  }

  // Add only the pairs (new_event, prior_events[i]) once each (symmetric).
  for (size_t i = 0; i < prior_events.size(); ++i) {
    const auto& prior = prior_events[i];
    if (prior.item_id == new_event.item_id) {
      continue;  // Skip self-pairs
    }

    auto score = static_cast<float>(new_event.score * prior.score);
    if (!prior_decay.empty()) {
      score *= new_decay * prior_decay[i];
    }

    co_scores_[new_event.item_id][prior.item_id] += score;
    co_scores_[prior.item_id][new_event.item_id] += score;
  }

  generation_.fetch_add(1, std::memory_order_release);

  // Only the new event's neighbor list can grow here, so prune just that item.
  if (config_.max_neighbors_per_item > 0) {
    auto it = co_scores_.find(new_event.item_id);
    if (it != co_scores_.end() && it->second.size() > config_.max_neighbors_per_item) {
      PruneItemLocked(new_event.item_id);
    }
  }
}

void CoOccurrenceIndex::UpdateFromEventsInternal(const std::string& ctx [[maybe_unused]],
                                                 const std::vector<Event>& events, bool temporal_enabled,
                                                 double half_life_sec) {
  // Pre-compute per-event decay weights
  std::vector<float> decay_weights;
  if (temporal_enabled && half_life_sec > 0.0) {
    uint64_t max_ts = 0;
    for (const auto& e : events) {
      if (e.timestamp > max_ts) {
        max_ts = e.timestamp;
      }
    }

    decay_weights.resize(events.size());
    const double inv_half_life = 1.0 / half_life_sec;
    constexpr float kMinDecay = 1e-6F;
    for (size_t i = 0; i < events.size(); ++i) {
      double age = static_cast<double>(max_ts - events[i].timestamp);
      auto decay = static_cast<float>(std::exp2(-age * inv_half_life));
      decay_weights[i] = std::max(decay, kMinDecay);
    }
  }

  // Compute pairwise co-occurrence scores
  for (size_t i = 0; i < events.size(); ++i) {
    for (size_t j = i + 1; j < events.size(); ++j) {
      const auto& event1 = events[i];
      const auto& event2 = events[j];

      if (event1.item_id == event2.item_id) {
        continue;
      }

      auto score = static_cast<float>(event1.score * event2.score);

      // Apply temporal decay if enabled
      if (!decay_weights.empty()) {
        score *= decay_weights[i] * decay_weights[j];
      }

      co_scores_[event1.item_id][event2.item_id] += score;
      co_scores_[event2.item_id][event1.item_id] += score;
    }
  }

  generation_.fetch_add(1, std::memory_order_release);

  // Prune affected items if max_neighbors is configured
  if (config_.max_neighbors_per_item > 0) {
    for (size_t i = 0; i < events.size(); ++i) {
      auto it = co_scores_.find(events[i].item_id);
      if (it != co_scores_.end() && it->second.size() > config_.max_neighbors_per_item) {
        PruneItemLocked(events[i].item_id);
      }
    }
  }
}

void CoOccurrenceIndex::ApplyNegativeSignalLocked(const std::string& removed_id,
                                                  const std::vector<Event>& context_events, double negative_weight) {
  // Skip if negative signal propagation is disabled
  if (config_.negative_max_propagation == 0) {
    return;
  }

  for (const auto& event : context_events) {
    if (event.item_id == removed_id) {
      continue;
    }

    auto reduction = static_cast<float>(event.score * negative_weight);

    // Symmetric reduction (both directions)
    co_scores_[removed_id][event.item_id] -= reduction;
    co_scores_[event.item_id][removed_id] -= reduction;
  }

  generation_.fetch_add(1, std::memory_order_release);
}

size_t CoOccurrenceIndex::GetNeighborCount(const std::string& item_id) const {
  std::shared_lock lock(mutex_);
  auto it = co_scores_.find(item_id);
  if (it == co_scores_.end()) {
    return 0;
  }
  return it->second.size();
}

std::vector<std::pair<std::string, float>> CoOccurrenceIndex::GetSimilar(const std::string& item_id, int top_k) const {
  if (top_k <= 0) {
    return {};
  }

  std::shared_lock lock(mutex_);

  auto iter = co_scores_.find(item_id);
  if (iter == co_scores_.end()) {
    return {};
  }

  // Collect all co-occurring items with scores
  std::vector<std::pair<std::string, float>> results;
  results.reserve(iter->second.size());

  for (const auto& [other_id, score] : iter->second) {
    if (score > 0.0F) {  // Only include positive scores
      results.emplace_back(other_id, score);
    }
  }

  // Sort by score descending
  std::sort(results.begin(), results.end(), [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });

  // Return top-k results
  if (results.size() > static_cast<size_t>(top_k)) {
    results.resize(static_cast<size_t>(top_k));
  }

  return results;
}

float CoOccurrenceIndex::GetScore(const std::string& item_id_1, const std::string& item_id_2) const {
  std::shared_lock lock(mutex_);

  auto iter1 = co_scores_.find(item_id_1);
  if (iter1 == co_scores_.end()) {
    return 0.0F;
  }

  auto iter2 = iter1->second.find(item_id_2);
  if (iter2 == iter1->second.end()) {
    return 0.0F;
  }

  return iter2->second;
}

void CoOccurrenceIndex::ApplyDecay(double alpha) {
  if (alpha <= 0.0 || alpha > 1.0) {
    return;  // Invalid alpha, skip decay
  }

  std::unique_lock lock(mutex_);

  constexpr float kDecayThreshold = 1e-6F;
  float threshold = (config_.min_support > 0.0F) ? config_.min_support : kDecayThreshold;

  for (auto outer_it = co_scores_.begin(); outer_it != co_scores_.end();) {
    auto& scores_map = outer_it->second;
    for (auto inner_it = scores_map.begin(); inner_it != scores_map.end();) {
      inner_it->second *= static_cast<float>(alpha);
      if (std::abs(inner_it->second) < threshold) {
        inner_it = scores_map.erase(inner_it);
      } else {
        ++inner_it;
      }
    }

    // Remove outer entry if no neighbors remain
    if (scores_map.empty()) {
      outer_it = co_scores_.erase(outer_it);
    } else {
      ++outer_it;
    }
  }

  generation_.fetch_add(1, std::memory_order_release);
}

size_t CoOccurrenceIndex::GetItemCount() const {
  std::shared_lock lock(mutex_);
  return co_scores_.size();
}

std::vector<std::string> CoOccurrenceIndex::GetAllItems() const {
  std::shared_lock lock(mutex_);

  std::vector<std::string> items;
  items.reserve(co_scores_.size());

  for (const auto& [item_id, _] : co_scores_) {
    items.push_back(item_id);
  }

  return items;
}

void CoOccurrenceIndex::SetScore(const std::string& item1, const std::string& item2, float score) {
  std::unique_lock lock(mutex_);
  co_scores_[item1][item2] = score;
  co_scores_[item2][item1] = score;
  generation_.fetch_add(1, std::memory_order_release);
}

void CoOccurrenceIndex::Prune() {
  std::unique_lock lock(mutex_);

  // Collect item IDs first (iteration will modify the map)
  std::vector<std::string> items;
  items.reserve(co_scores_.size());
  for (const auto& [id, _] : co_scores_) {
    items.push_back(id);
  }

  for (const auto& id : items) {
    PruneItemLocked(id);
  }

  generation_.fetch_add(1, std::memory_order_release);
}

void CoOccurrenceIndex::PruneItemLocked(const std::string& item_id) {
  auto it = co_scores_.find(item_id);
  if (it == co_scores_.end()) {
    return;
  }

  auto& neighbors = it->second;

  // Remove entries below min_support
  if (config_.min_support > 0.0F) {
    for (auto nit = neighbors.begin(); nit != neighbors.end();) {
      if (std::abs(nit->second) < config_.min_support) {
        // Also remove reverse entry
        auto rev_it = co_scores_.find(nit->first);
        if (rev_it != co_scores_.end()) {
          rev_it->second.erase(item_id);
          if (rev_it->second.empty()) {
            co_scores_.erase(rev_it);
          }
        }
        nit = neighbors.erase(nit);
      } else {
        ++nit;
      }
    }
  }

  // Trim to max_neighbors if needed
  if (config_.max_neighbors_per_item > 0 && neighbors.size() > config_.max_neighbors_per_item) {
    // Collect into vector, sort by absolute score descending, keep top-K
    std::vector<std::pair<std::string, float>> sorted_neighbors(neighbors.begin(), neighbors.end());
    std::sort(sorted_neighbors.begin(), sorted_neighbors.end(),
              [](const auto& a, const auto& b) { return std::abs(a.second) > std::abs(b.second); });

    // Remove entries beyond max_neighbors
    for (size_t i = config_.max_neighbors_per_item; i < sorted_neighbors.size(); ++i) {
      const auto& removed_id = sorted_neighbors[i].first;
      neighbors.erase(removed_id);
      // Also remove reverse entry
      auto rev_it = co_scores_.find(removed_id);
      if (rev_it != co_scores_.end()) {
        rev_it->second.erase(item_id);
        if (rev_it->second.empty()) {
          co_scores_.erase(rev_it);
        }
      }
    }
  }

  if (neighbors.empty()) {
    co_scores_.erase(it);
  }
}

void CoOccurrenceIndex::Clear() {
  std::unique_lock lock(mutex_);
  co_scores_.clear();
  generation_.fetch_add(1, std::memory_order_release);
}

CoOccurrenceIndexStatistics CoOccurrenceIndex::GetStatistics() const {
  std::shared_lock lock(mutex_);

  CoOccurrenceIndexStatistics stats;
  stats.tracked_ids = co_scores_.size();

  // Count co-occurrence pairs
  size_t pairs = 0;
  for (const auto& [item_id_1, neighbors] : co_scores_) {
    pairs += neighbors.size();
  }
  stats.co_pairs = pairs / 2;  // Symmetric matrix, so divide by 2

  // Reuse the lock we already hold; MemoryUsage() would re-lock the
  // non-recursive shared_mutex (undefined behavior / potential deadlock).
  stats.memory_bytes = MemoryUsageLocked();

  return stats;
}

size_t CoOccurrenceIndex::MemoryUsage() const {
  std::shared_lock lock(mutex_);
  return MemoryUsageLocked();
}

size_t CoOccurrenceIndex::MemoryUsageLocked() const {
  size_t total = 0;

  // Base container overhead
  total += sizeof(*this);

  // Co-occurrence matrix
  for (const auto& [item_id_1, neighbors] : co_scores_) {
    // Outer map key (item_id_1)
    total += sizeof(std::string) + item_id_1.capacity();

    // Inner map
    total += sizeof(std::unordered_map<std::string, float>);
    for (const auto& [item_id_2, score] : neighbors) {
      total += sizeof(std::string) + item_id_2.capacity();
      total += sizeof(float);
    }
  }

  return total;
}

std::shared_lock<std::shared_mutex> CoOccurrenceIndex::AcquireReadLock() const {
  return std::shared_lock<std::shared_mutex>(mutex_);
}

std::unique_lock<std::shared_mutex> CoOccurrenceIndex::AcquireWriteLock() {
  return std::unique_lock<std::shared_mutex>(mutex_);
}

}  // namespace nvecd::events

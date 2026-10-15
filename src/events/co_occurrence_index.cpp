/**
 * @file co_occurrence_index.cpp
 * @brief Co-occurrence index implementation
 */

#include "events/co_occurrence_index.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "events/id_validation.h"

namespace nvecd::events {

namespace {

using CoScoreMap = std::unordered_map<std::string, std::unordered_map<std::string, float>>;

// Decay is pair-local, so a streaming update stays bit-for-bit equal to a batch rebuild in any arrival order.
float PairContribution(const Event& first, const Event& second, bool temporal_enabled, double inv_half_life) {
  // Widen first: snapshot restore can supply scores outside [0,100], and int*int could overflow.
  float score = static_cast<float>(static_cast<int64_t>(first.score) * second.score);
  if (!temporal_enabled) {
    return score;
  }

  // Skip the newer endpoint (decay one): exp2(-0 * inf) is NaN for a subnormal half-life.
  const uint64_t newer_timestamp = std::max(first.timestamp, second.timestamp);
  const uint64_t older_timestamp = std::min(first.timestamp, second.timestamp);
  if (newer_timestamp != older_timestamp) {
    constexpr float kMinDecay = 1e-6F;
    const double age = static_cast<double>(newer_timestamp - older_timestamp);
    const float decay = std::max(static_cast<float>(std::exp2(-age * inv_half_life)), kMinDecay);
    score *= decay;
  }
  return score;
}

bool AddDirectedScore(CoScoreMap& scores, const std::string& item_id, const std::string& neighbor_id, float delta) {
  if (delta == 0.0F) {
    return false;
  }

  auto [item_it, item_inserted] = scores.try_emplace(item_id);
  auto [neighbor_it, neighbor_inserted] = item_it->second.try_emplace(neighbor_id, 0.0F);
  const float updated = neighbor_it->second + delta;
  if (updated == neighbor_it->second) {
    if (neighbor_inserted) {
      item_it->second.erase(neighbor_it);
      if (item_inserted) {
        scores.erase(item_it);
      }
    }
    return false;
  }
  neighbor_it->second = updated;
  return true;
}

bool AddSymmetricScore(CoScoreMap& scores, const std::string& item1, const std::string& item2, float delta) {
  const bool forward_changed = AddDirectedScore(scores, item1, item2, delta);
  const bool reverse_changed = AddDirectedScore(scores, item2, item1, delta);
  return forward_changed || reverse_changed;
}

}  // namespace

utils::Expected<ValidatedEdge, utils::Error> CoOccurrenceEdgeValidator::Validate(const std::string& item1,
                                                                                 const std::string& item2,
                                                                                 float score) {
  auto item1_valid = ValidateIdentifier("ID", item1);
  if (!item1_valid) {
    return utils::MakeUnexpected(item1_valid.error());
  }
  auto item2_valid = ValidateIdentifier("ID", item2);
  if (!item2_valid) {
    return utils::MakeUnexpected(item2_valid.error());
  }
  if (item1 == item2) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kEventCoOccurrenceError,
                                                  "Co-occurrence edge cannot join an item to itself: " + item1));
  }
  if (!std::isfinite(score)) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kEventCoOccurrenceError, "Co-occurrence score must be finite"));
  }

  return ValidatedEdge(CoOccurrenceEdge{item1, item2, score});
}

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
  // Ingestion holds to the same identifier rules as the direct writers, so a
  // caller assembling events itself cannot introduce a pair the restore path
  // would reject.
  if (!ValidateIdentifier("ID", new_event.item_id)) {
    return;
  }

  const bool apply_temporal_decay = temporal_enabled && half_life_sec > 0.0;
  const double inv_half_life = apply_temporal_decay ? 1.0 / half_life_sec : 0.0;
  bool state_changed = false;

  // Add only the pairs (new_event, prior_events[i]) once each (symmetric).
  for (size_t i = 0; i < prior_events.size(); ++i) {
    const auto& prior = prior_events[i];
    if (prior.item_id == new_event.item_id) {
      continue;  // Skip self-pairs
    }
    if (!ValidateIdentifier("ID", prior.item_id)) {
      continue;
    }

    // Pair-local scoring keeps incremental updates equivalent to the batch
    // rebuild regardless of event arrival order.
    const float score = PairContribution(new_event, prior, apply_temporal_decay, inv_half_life);

    // A zero contribution must not materialize a pair.  Besides wasting
    // memory, such entries inflate neighbor/item statistics even though they
    // can never appear in a similarity result.
    if (score == 0.0F) {
      continue;
    }

    state_changed = AddSymmetricScore(co_scores_, new_event.item_id, prior.item_id, score) || state_changed;
  }

  // Both endpoints of every new edge grow. In particular, a frequently seen
  // hub can exceed its cap while each newly ingested leaf remains small.
  // Prune every affected ID under the same lock so the symmetric matrix never
  // exposes an over-limit existing endpoint after this update returns.
  std::vector<std::string> affected_ids;
  affected_ids.reserve(prior_events.size() + 1);
  affected_ids.push_back(new_event.item_id);
  for (const auto& prior : prior_events) {
    affected_ids.push_back(prior.item_id);
  }
  state_changed = PruneAffectedLocked(std::move(affected_ids)) || state_changed;
  if (state_changed) {
    generation_.fetch_add(1, std::memory_order_release);
  }
}

void CoOccurrenceIndex::UpdateFromEventsInternal(const std::string& ctx [[maybe_unused]],
                                                 const std::vector<Event>& events, bool temporal_enabled,
                                                 double half_life_sec) {
  // Identifiers are checked once per event rather than once per pair: the rules
  // are the ones the direct writers enforce, so a caller assembling events
  // itself cannot introduce a pair the restore path would reject.
  std::vector<char> usable(events.size(), 0);
  for (size_t i = 0; i < events.size(); ++i) {
    usable[i] = static_cast<char>(ValidateIdentifier("ID", events[i].item_id).has_value());
  }

  const bool apply_temporal_decay = temporal_enabled && half_life_sec > 0.0;
  const double inv_half_life = apply_temporal_decay ? 1.0 / half_life_sec : 0.0;
  bool state_changed = false;

  // Compute pairwise co-occurrence scores
  for (size_t i = 0; i < events.size(); ++i) {
    if (usable[i] == 0) {
      continue;
    }
    for (size_t j = i + 1; j < events.size(); ++j) {
      if (usable[j] == 0) {
        continue;
      }
      const auto& event1 = events[i];
      const auto& event2 = events[j];

      if (event1.item_id == event2.item_id) {
        continue;
      }

      const float score = PairContribution(event1, event2, apply_temporal_decay, inv_half_life);

      // Do not create map entries for a non-contribution (for example an
      // event with score zero).
      if (score == 0.0F) {
        continue;
      }

      state_changed = AddSymmetricScore(co_scores_, event1.item_id, event2.item_id, score) || state_changed;
    }
  }

  // Prune affected items after a bulk update. Apply min_support immediately
  // as well, otherwise a deployment with decay disabled never enforces it.
  std::vector<std::string> affected_ids;
  affected_ids.reserve(events.size());
  for (const auto& event : events) {
    affected_ids.push_back(event.item_id);
  }
  state_changed = PruneAffectedLocked(std::move(affected_ids)) || state_changed;
  if (state_changed) {
    generation_.fetch_add(1, std::memory_order_release);
  }
}

void CoOccurrenceIndex::ApplyNegativeSignalLocked(const std::string& removed_id,
                                                  const std::vector<Event>& context_events, double negative_weight) {
  // Skip if negative signal propagation is disabled
  if (config_.negative_max_propagation == 0) {
    return;
  }

  if (!ValidateIdentifier("ID", removed_id)) {
    return;
  }

  bool state_changed = false;
  std::vector<std::string> affected_ids{removed_id};
  for (const auto& event : context_events) {
    if (event.item_id == removed_id) {
      continue;
    }
    if (!ValidateIdentifier("ID", event.item_id)) {
      continue;
    }

    auto reduction = static_cast<float>(event.score * negative_weight);

    // A zero reduction must not materialize an edge, as in the positive path.
    if (reduction == 0.0F) {
      continue;
    }

    // Symmetric reduction (both directions)
    state_changed = AddSymmetricScore(co_scores_, removed_id, event.item_id, -reduction) || state_changed;
    affected_ids.push_back(event.item_id);
  }

  // A reduction can create an edge that did not exist, so the retention bounds
  // apply to every ID it touched, in the same write.
  state_changed = PruneAffectedLocked(std::move(affected_ids)) || state_changed;
  if (state_changed) {
    generation_.fetch_add(1, std::memory_order_release);
  }
}

bool CoOccurrenceIndex::PruneAffectedLocked(std::vector<std::string> affected_ids) {
  if (config_.max_neighbors_per_item == 0 && !(config_.min_support > 0.0F)) {
    return false;
  }
  std::sort(affected_ids.begin(), affected_ids.end());
  affected_ids.erase(std::unique(affected_ids.begin(), affected_ids.end()), affected_ids.end());
  bool changed = false;
  for (const auto& item_id : affected_ids) {
    changed = PruneItemLocked(item_id) || changed;
  }
  return changed;
}

size_t CoOccurrenceIndex::GetNeighborCount(const std::string& item_id) const {
  std::shared_lock lock(mutex_);
  auto it = co_scores_.find(item_id);
  if (it == co_scores_.end()) {
    return 0;
  }
  return it->second.size();
}

size_t CoOccurrenceIndex::GetPositiveNeighborCount(const std::string& item_id) const {
  std::shared_lock lock(mutex_);
  auto it = co_scores_.find(item_id);
  if (it == co_scores_.end()) {
    return 0;
  }
  return static_cast<size_t>(
      std::count_if(it->second.begin(), it->second.end(), [](const auto& entry) { return entry.second > 0.0F; }));
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

  // Rank by pointer so only the kept neighbours are copied.
  std::vector<std::pair<float, const std::string*>> ranked;
  ranked.reserve(iter->second.size());

  for (const auto& [other_id, score] : iter->second) {
    if (score > 0.0F) {  // Only include positive scores
      ranked.emplace_back(score, &other_id);
    }
  }

  // Score descending, ties by ascending id: the selected set at the top-k
  // boundary must not depend on hash iteration order.
  const size_t keep = std::min(ranked.size(), static_cast<size_t>(top_k));
  std::partial_sort(ranked.begin(), ranked.begin() + static_cast<ptrdiff_t>(keep), ranked.end(),
                    [](const auto& lhs, const auto& rhs) {
                      if (lhs.first != rhs.first) {
                        return lhs.first > rhs.first;
                      }
                      return *lhs.second < *rhs.second;
                    });

  std::vector<std::pair<std::string, float>> results;
  results.reserve(keep);
  for (size_t i = 0; i < keep; ++i) {
    results.emplace_back(*ranked[i].second, ranked[i].first);
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
  if (!IsValidDecayAlpha(alpha)) {
    return;  // Invalid alpha, skip decay
  }

  std::unique_lock lock(mutex_);
  if (alpha == 0.0) {
    if (!co_scores_.empty()) {
      co_scores_.clear();
      generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    return;
  }

  constexpr float kDecayThreshold = 1e-6F;
  float threshold = (config_.min_support > 0.0F) ? config_.min_support : kDecayThreshold;
  bool state_changed = false;

  for (auto outer_it = co_scores_.begin(); outer_it != co_scores_.end();) {
    auto& scores_map = outer_it->second;
    for (auto inner_it = scores_map.begin(); inner_it != scores_map.end();) {
      const float previous = inner_it->second;
      inner_it->second *= static_cast<float>(alpha);
      if (std::abs(inner_it->second) < threshold) {
        inner_it = scores_map.erase(inner_it);
        state_changed = true;
      } else {
        state_changed = (inner_it->second != previous) || state_changed;
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

  if (state_changed) {
    generation_.fetch_add(1, std::memory_order_release);
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

  for (const auto& [item_id, _] : co_scores_) {
    items.push_back(item_id);
  }

  return items;
}

std::vector<std::pair<std::string, float>> CoOccurrenceIndex::GetAllNeighbors(const std::string& item_id) const {
  std::shared_lock lock(mutex_);

  auto iter = co_scores_.find(item_id);
  if (iter == co_scores_.end()) {
    return {};
  }

  // Return every recorded neighbor unfiltered (including zero/negative scores)
  // so serialization preserves negative-signal baselines exactly.
  std::vector<std::pair<std::string, float>> results;
  results.reserve(iter->second.size());
  for (const auto& [other_id, score] : iter->second) {
    results.emplace_back(other_id, score);
  }
  return results;
}

utils::Expected<void, utils::Error> CoOccurrenceIndex::SetScore(const std::string& item1, const std::string& item2,
                                                                float score) {
  auto validated = CoOccurrenceEdgeValidator::Validate(item1, item2, score);
  if (!validated) {
    return utils::MakeUnexpected(validated.error());
  }
  SetValidatedScore(*validated);
  return {};
}

void CoOccurrenceIndex::SetValidatedScore(const ValidatedEdge& edge) {
  std::unique_lock lock(mutex_);
  const auto existing1 = co_scores_.find(edge->item1);
  if (existing1 != co_scores_.end()) {
    const auto score12 = existing1->second.find(edge->item2);
    const auto existing2 = co_scores_.find(edge->item2);
    if (score12 != existing1->second.end() && existing2 != co_scores_.end()) {
      const auto score21 = existing2->second.find(edge->item1);
      if (score21 != existing2->second.end() && score12->second == edge->score && score21->second == edge->score) {
        return;
      }
    }
  }
  co_scores_[edge->item1][edge->item2] = edge->score;
  co_scores_[edge->item2][edge->item1] = edge->score;
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

  bool state_changed = false;
  for (const auto& id : items) {
    state_changed = PruneItemLocked(id) || state_changed;
  }

  if (state_changed) {
    generation_.fetch_add(1, std::memory_order_release);
  }
}

bool CoOccurrenceIndex::PruneItemLocked(const std::string& item_id) {
  auto it = co_scores_.find(item_id);
  if (it == co_scores_.end()) {
    return false;
  }

  auto& neighbors = it->second;
  bool state_changed = false;

  // Remove entries below min_support
  if (config_.min_support > 0.0F) {
    for (auto nit = neighbors.begin(); nit != neighbors.end();) {
      if (std::abs(nit->second) < config_.min_support) {
        // Also remove reverse entry. A self-edge would make the reverse entry
        // this very node: erasing it here would invalidate `neighbors` and the
        // iterator still being walked, so leave the node to the single erase at
        // the end of this function.
        auto rev_it = co_scores_.find(nit->first);
        if (rev_it != co_scores_.end() && rev_it != it) {
          rev_it->second.erase(item_id);
          if (rev_it->second.empty()) {
            co_scores_.erase(rev_it);
          }
        }
        nit = neighbors.erase(nit);
        state_changed = true;
      } else {
        ++nit;
      }
    }
  }

  // Trim to max_neighbors if needed
  if (config_.max_neighbors_per_item > 0 && neighbors.size() > config_.max_neighbors_per_item) {
    // Deterministic top-K by |score| descending, ties by ascending ID; the tail stays unordered.
    std::vector<std::pair<std::string, float>> sorted_neighbors(neighbors.begin(), neighbors.end());
    const auto ranks_before = [](const auto& a, const auto& b) {
      const float score_a = std::abs(a.second);
      const float score_b = std::abs(b.second);
      if (score_a != score_b) {
        return score_a > score_b;
      }
      return a.first < b.first;
    };
    const auto keep_end = sorted_neighbors.begin() + config_.max_neighbors_per_item;
    std::partial_sort(sorted_neighbors.begin(), keep_end, sorted_neighbors.end(), ranks_before);

    // Remove entries beyond max_neighbors
    for (size_t i = config_.max_neighbors_per_item; i < sorted_neighbors.size(); ++i) {
      const auto& removed_id = sorted_neighbors[i].first;
      neighbors.erase(removed_id);
      state_changed = true;
      // Also remove reverse entry, except when it is this node (self-edge):
      // erasing it here would leave `neighbors` dangling for the rest of the
      // loop, and the trailing erase below already covers that case.
      auto rev_it = co_scores_.find(removed_id);
      if (rev_it != co_scores_.end() && rev_it != it) {
        rev_it->second.erase(item_id);
        if (rev_it->second.empty()) {
          co_scores_.erase(rev_it);
        }
      }
    }
  }

  if (neighbors.empty()) {
    co_scores_.erase(it);
    state_changed = true;
  }
  return state_changed;
}

void CoOccurrenceIndex::Clear() {
  std::unique_lock lock(mutex_);
  if (co_scores_.empty()) {
    return;
  }
  co_scores_.clear();
  generation_.fetch_add(1, std::memory_order_release);
}

void CoOccurrenceIndex::SwapState(CoOccurrenceIndex& other) {
  if (this == &other) {
    return;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  co_scores_.swap(other.co_scores_);
  const uint64_t this_generation = generation_.load(std::memory_order_relaxed);
  generation_.store(other.generation_.load(std::memory_order_relaxed), std::memory_order_release);
  other.generation_.store(this_generation, std::memory_order_release);
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

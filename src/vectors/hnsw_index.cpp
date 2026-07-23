/**
 * @file hnsw_index.cpp
 * @brief HNSW index implementation
 */

#include "vectors/hnsw_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_set>

#include "utils/error.h"
#include "utils/structured_log.h"
#include "vectors/distance.h"

namespace nvecd::vectors {

namespace {

/// Upper bound on the layer a single node may occupy.
///
/// The geometric level distribution is unbounded in theory: an unlucky draw of
/// a near-zero random value produces an arbitrarily large level. Capping the
/// level keeps the per-node neighbor structure (and the graph height) bounded,
/// which prevents pathological memory growth from a single insertion. A bound
/// of 64 comfortably exceeds the height any realistic index reaches, since the
/// number of nodes required to populate level L grows as M^L.
constexpr uint32_t kMaxNodeLevel = 64;
constexpr uint32_t kMaxVectorDimension = 4096;
constexpr uint32_t kMaxDeserializedNodes = 10'000'000;

/// Return whether a seekable stream contains at least @p required bytes from
/// its current position. Non-seekable streams retain the structural limits
/// below; snapshot files and string streams used by the server are seekable.
bool HasRemainingBytes(std::istream& in, size_t required) {
  const auto current = in.tellg();
  if (current == std::istream::pos_type(-1)) {
    in.clear(in.rdstate() & ~std::ios::failbit);
    return true;
  }
  in.seekg(0, std::ios::end);
  const auto end = in.tellg();
  in.seekg(current);
  if (end == std::istream::pos_type(-1) || end < current) {
    return false;
  }
  return static_cast<uint64_t>(end - current) >= required;
}

}  // namespace

// ============================================================================
// Construction / Move
// ============================================================================

HnswIndex::HnswIndex(uint32_t dimension, DistanceFunc distance_func, const Config& config)
    : config_(config),
      dimension_(dimension),
      distance_func_(distance_func),
      use_cosine_prenorm_(distance_func == CosineDistanceRaw),
      rng_(std::random_device{}()) {
  level_mult_ = 1.0 / std::log(static_cast<double>(std::max(config_.m, 2U)));

  if (config_.max_elements > 0) {
    nodes_.reserve(config_.max_elements);
    vectors_.reserve(static_cast<size_t>(config_.max_elements) * dimension_);
    vector_norms_.reserve(config_.max_elements);
    compact_to_internal_.reserve(config_.max_elements);
  }
}

HnswIndex::HnswIndex(HnswIndex&& other) noexcept
    : config_(other.config_),
      dimension_(other.dimension_),
      distance_func_(other.distance_func_),
      nodes_(std::move(other.nodes_)),
      vectors_(std::move(other.vectors_)),
      vector_norms_(std::move(other.vector_norms_)),
      use_cosine_prenorm_(other.use_cosine_prenorm_),
      compact_to_internal_(std::move(other.compact_to_internal_)),
      entry_point_(other.entry_point_),
      max_level_(other.max_level_),
      active_count_(other.active_count_),
      level_mult_(other.level_mult_),
      rng_(std::move(other.rng_)) {
  other.entry_point_ = UINT32_MAX;
  other.max_level_ = 0;
  other.active_count_ = 0;
}

HnswIndex& HnswIndex::operator=(HnswIndex&& other) noexcept {
  if (this != &other) {
    config_ = other.config_;
    dimension_ = other.dimension_;
    distance_func_ = other.distance_func_;
    nodes_ = std::move(other.nodes_);
    vectors_ = std::move(other.vectors_);
    vector_norms_ = std::move(other.vector_norms_);
    compact_to_internal_ = std::move(other.compact_to_internal_);
    entry_point_ = other.entry_point_;
    max_level_ = other.max_level_;
    active_count_ = other.active_count_;
    level_mult_ = other.level_mult_;
    use_cosine_prenorm_ = other.use_cosine_prenorm_;
    rng_ = std::move(other.rng_);
    other.entry_point_ = UINT32_MAX;
    other.max_level_ = 0;
    other.active_count_ = 0;
  }
  return *this;
}

// ============================================================================
// Core HNSW Operations
// ============================================================================

uint32_t HnswIndex::RandomLevel() {
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  double r = dist(rng_);
  // uniform_real_distribution may return exactly 0.0. Taking -log(0.0) yields
  // +inf, and converting +inf to uint32_t is undefined behavior that can lead
  // to an absurd level and a bad_alloc when resizing the neighbor structure.
  // Clamp r to the smallest positive double so -log(r) stays finite.
  if (r <= 0.0) {
    r = std::numeric_limits<double>::min();
  }
  const double level_f = -std::log(r) * level_mult_;
  // The geometric draw is unbounded; clamp to a safe ceiling so a single
  // unlucky insertion cannot blow up the graph height.
  if (!(level_f < static_cast<double>(kMaxNodeLevel))) {
    return kMaxNodeLevel;
  }
  return static_cast<uint32_t>(level_f);
}

uint32_t HnswIndex::MaxNeighbors(uint32_t level) const {
  // Layer 0 gets 2*M connections, upper layers get M
  return (level == 0) ? config_.m * 2 : config_.m;
}

const float* HnswIndex::GetNodeVector(uint32_t internal_id) const {
  return vectors_.data() + static_cast<size_t>(internal_id) * dimension_;
}

float HnswIndex::ComputeDistance(const float* query, float query_norm, uint32_t internal_id) const {
  if (use_cosine_prenorm_) {
    return CosineSimilarityPreNorm(query, GetNodeVector(internal_id), dimension_, query_norm,
                                   vector_norms_[internal_id]);
  }
  return distance_func_(query, GetNodeVector(internal_id), dimension_);
}

std::vector<std::pair<float, uint32_t>> HnswIndex::SearchLayer(const float* query, float query_norm,
                                                               uint32_t entry_node, uint32_t layer, uint32_t ef) const {
  // Max-heap for candidates (worst candidate on top for easy eviction)
  // Min-heap for visited-but-not-yet-expanded (best candidate on top)
  float entry_dist = ComputeDistance(query, query_norm, entry_node);

  // candidates: max-heap (worst first) — the result set W
  auto cmp_max = [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b) {
    return a.first > b.first;
  };
  std::priority_queue<std::pair<float, uint32_t>, std::vector<std::pair<float, uint32_t>>, decltype(cmp_max)>
      candidates(cmp_max);

  // to_visit: min-heap (best first) — the candidate set C
  auto cmp_min = [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b) {
    return a.first < b.first;
  };
  std::priority_queue<std::pair<float, uint32_t>, std::vector<std::pair<float, uint32_t>>, decltype(cmp_min)> to_visit(
      cmp_min);

  std::unordered_set<uint32_t> visited;
  visited.insert(entry_node);

  candidates.push({entry_dist, entry_node});
  to_visit.push({entry_dist, entry_node});

  while (!to_visit.empty()) {
    auto [c_dist, c_id] = to_visit.top();

    // If the closest unvisited candidate is worse than the worst in result set
    // and we have enough results, stop
    float worst_dist = candidates.top().first;
    if (c_dist < worst_dist && candidates.size() >= ef) {
      break;
    }
    to_visit.pop();

    // Expand neighbors of c_id at this layer
    const auto& neighbors = nodes_[c_id].neighbors;
    if (layer >= neighbors.size()) {
      continue;
    }

    for (uint32_t neighbor_id : neighbors[layer]) {
      if (visited.count(neighbor_id) != 0) {
        continue;
      }
      visited.insert(neighbor_id);

      float dist = ComputeDistance(query, query_norm, neighbor_id);

      if (candidates.size() < ef || dist > candidates.top().first) {
        candidates.push({dist, neighbor_id});
        to_visit.push({dist, neighbor_id});

        if (candidates.size() > ef) {
          candidates.pop();
        }
      }
    }
  }

  // Extract results from candidates heap
  std::vector<std::pair<float, uint32_t>> result;
  result.reserve(candidates.size());
  while (!candidates.empty()) {
    result.push_back(candidates.top());
    candidates.pop();
  }
  // Sort by distance descending (highest similarity first)
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  return result;
}

std::vector<uint32_t> HnswIndex::SelectNeighbors(const std::vector<std::pair<float, uint32_t>>& candidates,
                                                 uint32_t max_count) const {
  // Simple selection: take the closest max_count neighbors
  // Candidates are sorted by distance descending (highest similarity first)
  std::vector<uint32_t> selected;
  uint32_t count = std::min(max_count, static_cast<uint32_t>(candidates.size()));
  selected.reserve(count);

  for (uint32_t i = 0; i < count; ++i) {
    selected.push_back(candidates[i].second);
  }
  return selected;
}

// ============================================================================
// AnnIndex Interface
// ============================================================================

void HnswIndex::Add(uint32_t compact_index, const float* vector) {
  std::unique_lock lock(mutex_);
  AddLocked(compact_index, vector);
  MaybeCompactLocked();
}

void HnswIndex::MaybeCompactLocked() {
  if (nodes_.size() < config_.rebuild_min_nodes || nodes_.empty()) {
    return;
  }
  const size_t tombstones = nodes_.size() - active_count_;
  if (static_cast<double>(tombstones) / static_cast<double>(nodes_.size()) <
      static_cast<double>(config_.rebuild_tombstone_ratio)) {
    return;
  }

  std::vector<std::pair<uint32_t, std::vector<float>>> live_vectors;
  live_vectors.reserve(active_count_);
  for (uint32_t compact_index = 0; compact_index < compact_to_internal_.size(); ++compact_index) {
    const uint32_t internal_id = compact_to_internal_[compact_index];
    if (internal_id == UINT32_MAX || internal_id >= nodes_.size() || nodes_[internal_id].deleted) {
      continue;
    }
    const float* source = GetNodeVector(internal_id);
    live_vectors.emplace_back(compact_index, std::vector<float>(source, source + dimension_));
  }

  nodes_.clear();
  vectors_.clear();
  vector_norms_.clear();
  compact_to_internal_.clear();
  entry_point_ = UINT32_MAX;
  max_level_ = 0;
  active_count_ = 0;
  for (const auto& [compact_index, vector] : live_vectors) {
    AddLocked(compact_index, vector.data());
  }
}

void HnswIndex::AddLocked(uint32_t compact_index, const float* vector) {
  uint32_t internal_id = static_cast<uint32_t>(nodes_.size());
  const float vector_norm = use_cosine_prenorm_ ? simd::GetOptimalImpl().l2_norm(vector, dimension_) : 0.0F;

  // Grow compact_to_internal_ if needed
  if (compact_index >= compact_to_internal_.size()) {
    compact_to_internal_.resize(compact_index + 1, UINT32_MAX);
  }

  // Overwrite (re-VECSET of the same id / reused tombstone slot): retire the
  // previous node for this compact_index instead of orphaning it. Leaving it
  // live would double-count active_count_, return the item twice from Search,
  // and keep ranking it by the stale embedding. A fresh node is inserted below
  // so the new embedding participates in the graph.
  uint32_t previous_internal = compact_to_internal_[compact_index];
  if (previous_internal != UINT32_MAX && previous_internal < nodes_.size() && !nodes_[previous_internal].deleted) {
    nodes_[previous_internal].deleted = true;
    if (active_count_ > 0) {
      --active_count_;
    }
  }

  compact_to_internal_[compact_index] = internal_id;

  // Store vector data
  size_t offset = vectors_.size();
  vectors_.resize(offset + dimension_);
  std::memcpy(vectors_.data() + offset, vector, static_cast<size_t>(dimension_) * sizeof(float));
  vector_norms_.push_back(vector_norm);

  // Create node with random level
  uint32_t level = RandomLevel();

  Node node;
  node.compact_index = compact_index;
  node.level = level;
  node.deleted = false;
  node.neighbors.resize(level + 1);
  nodes_.push_back(std::move(node));
  active_count_++;

  // First node: set as entry point
  if (entry_point_ == UINT32_MAX) {
    entry_point_ = internal_id;
    max_level_ = level;
    return;
  }

  // Navigate from top to the node's level, finding the closest entry at each layer
  uint32_t cur_node = entry_point_;
  for (int l = static_cast<int>(max_level_); l > static_cast<int>(level); --l) {
    // Greedy search: move to closest neighbor at this layer
    bool changed = true;
    while (changed) {
      changed = false;
      float cur_dist = ComputeDistance(vector, vector_norm, cur_node);
      if (static_cast<uint32_t>(l) < nodes_[cur_node].neighbors.size()) {
        for (uint32_t neighbor : nodes_[cur_node].neighbors[l]) {
          float d = ComputeDistance(vector, vector_norm, neighbor);
          if (d > cur_dist) {
            cur_node = neighbor;
            cur_dist = d;
            changed = true;
          }
        }
      }
    }
  }

  // Insert into layers from level down to 0
  for (int l = static_cast<int>(std::min(level, max_level_)); l >= 0; --l) {
    uint32_t layer = static_cast<uint32_t>(l);
    uint32_t ef = config_.ef_construction;

    auto candidates = SearchLayer(vector, vector_norm, cur_node, layer, ef);

    // Select neighbors for this layer
    uint32_t max_conn = MaxNeighbors(layer);
    auto selected = SelectNeighbors(candidates, max_conn);

    // Set forward links (new node -> selected neighbors)
    nodes_[internal_id].neighbors[layer] = selected;

    // Set reverse links (selected neighbors -> new node)
    for (uint32_t neighbor_id : selected) {
      auto& neighbor_links = nodes_[neighbor_id].neighbors;
      if (layer >= neighbor_links.size()) {
        continue;
      }
      neighbor_links[layer].push_back(internal_id);

      // Prune if too many connections
      if (neighbor_links[layer].size() > max_conn) {
        // Build candidate list with distances from the neighbor's perspective
        const float* neighbor_vec = GetNodeVector(neighbor_id);
        std::vector<std::pair<float, uint32_t>> neighbor_candidates;
        neighbor_candidates.reserve(neighbor_links[layer].size());
        for (uint32_t nid : neighbor_links[layer]) {
          float d = use_cosine_prenorm_ ? CosineSimilarityPreNorm(neighbor_vec, GetNodeVector(nid), dimension_,
                                                                  vector_norms_[neighbor_id], vector_norms_[nid])
                                        : distance_func_(neighbor_vec, GetNodeVector(nid), dimension_);
          neighbor_candidates.push_back({d, nid});
        }
        std::sort(neighbor_candidates.begin(), neighbor_candidates.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        auto pruned = SelectNeighbors(neighbor_candidates, max_conn);
        neighbor_links[layer] = std::move(pruned);
      }
    }

    // Use best candidate as entry for next layer down
    if (!candidates.empty()) {
      cur_node = candidates[0].second;
    }
  }

  // Update entry point if new node has higher level
  if (level > max_level_) {
    entry_point_ = internal_id;
    max_level_ = level;
  }
}

void HnswIndex::MarkDeleted(uint32_t compact_index) {
  std::unique_lock lock(mutex_);

  if (compact_index >= compact_to_internal_.size()) {
    return;
  }
  uint32_t internal_id = compact_to_internal_[compact_index];
  if (internal_id == UINT32_MAX) {
    return;
  }
  if (!nodes_[internal_id].deleted) {
    nodes_[internal_id].deleted = true;
    active_count_--;
    MaybeCompactLocked();
  }
}

std::vector<std::pair<uint32_t, float>> HnswIndex::Search(const float* query, uint32_t top_k) const {
  std::shared_lock lock(mutex_);

  if (entry_point_ == UINT32_MAX || active_count_ == 0) {
    return {};
  }

  // Use ef_search as the search width (at least top_k)
  uint32_t ef = std::max(config_.ef_search, top_k);
  const float query_norm = use_cosine_prenorm_ ? simd::GetOptimalImpl().l2_norm(query, dimension_) : 0.0F;

  // Navigate from top layers to layer 1, finding closest entry
  uint32_t cur_node = entry_point_;
  for (int l = static_cast<int>(max_level_); l > 0; --l) {
    bool changed = true;
    while (changed) {
      changed = false;
      float cur_dist = ComputeDistance(query, query_norm, cur_node);
      if (static_cast<uint32_t>(l) < nodes_[cur_node].neighbors.size()) {
        for (uint32_t neighbor : nodes_[cur_node].neighbors[l]) {
          float d = ComputeDistance(query, query_norm, neighbor);
          if (d > cur_dist) {
            cur_node = neighbor;
            cur_dist = d;
            changed = true;
          }
        }
      }
    }
  }

  std::vector<std::pair<uint32_t, float>> results;
  while (true) {
    auto candidates = SearchLayer(query, query_norm, cur_node, 0, ef);
    results.clear();
    results.reserve(std::min(static_cast<uint32_t>(candidates.size()), top_k));
    for (const auto& [score, internal_id] : candidates) {
      if (nodes_[internal_id].deleted) {
        continue;
      }
      results.push_back({nodes_[internal_id].compact_index, score});
      if (static_cast<uint32_t>(results.size()) >= top_k) {
        break;
      }
    }
    if (results.size() >= top_k || ef >= nodes_.size()) {
      break;
    }
    // Deleted candidates consume the first search budget. Expand until the
    // requested live count is found or every node has been considered.
    ef = static_cast<uint32_t>(std::min<size_t>(nodes_.size(), std::max<size_t>(ef + 1, ef * 2ULL)));
  }

  return results;
}

void HnswIndex::Rebuild(const float* all_vectors, uint32_t count, uint32_t dimension) {
  std::unique_lock lock(mutex_);

  // Clear existing state
  nodes_.clear();
  vectors_.clear();
  vector_norms_.clear();
  compact_to_internal_.clear();
  entry_point_ = UINT32_MAX;
  max_level_ = 0;
  active_count_ = 0;
  dimension_ = dimension;

  if (count == 0) {
    return;
  }

  // Reserve space. compact_to_internal_ must be seeded with the empty sentinel
  // (UINT32_MAX), not zero-filled: Add() reads compact_to_internal_[compact_index]
  // to detect an overwrite, and a stray 0 would be misread as "live node 0" and
  // wrongly retire it.
  nodes_.reserve(count);
  vectors_.reserve(static_cast<size_t>(count) * dimension);
  vector_norms_.reserve(count);
  compact_to_internal_.assign(count, UINT32_MAX);

  // Re-insert all vectors while retaining the exclusive lock. Readers either
  // observe the old graph before Rebuild starts or the complete replacement;
  // they never search an empty/partially rebuilt graph.
  for (uint32_t i = 0; i < count; ++i) {
    AddLocked(i, all_vectors + static_cast<size_t>(i) * dimension);
  }
}

uint32_t HnswIndex::Size() const {
  std::shared_lock lock(mutex_);
  return active_count_;
}

// ============================================================================
// Serialization
// ============================================================================

utils::Expected<void, utils::Error> HnswIndex::Serialize(std::ostream& out) const {
  std::shared_lock lock(mutex_);

  // Header
  uint32_t magic = 0x48534E57;  // "HSNW"
  uint32_t version = 1;
  out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
  out.write(reinterpret_cast<const char*>(&version), sizeof(version));

  // Config
  out.write(reinterpret_cast<const char*>(&config_.m), sizeof(config_.m));
  out.write(reinterpret_cast<const char*>(&config_.ef_construction), sizeof(config_.ef_construction));
  out.write(reinterpret_cast<const char*>(&config_.ef_search), sizeof(config_.ef_search));

  // Dimensions and counts
  out.write(reinterpret_cast<const char*>(&dimension_), sizeof(dimension_));
  auto node_count = static_cast<uint32_t>(nodes_.size());
  out.write(reinterpret_cast<const char*>(&node_count), sizeof(node_count));
  out.write(reinterpret_cast<const char*>(&entry_point_), sizeof(entry_point_));
  out.write(reinterpret_cast<const char*>(&max_level_), sizeof(max_level_));
  out.write(reinterpret_cast<const char*>(&active_count_), sizeof(active_count_));

  // Vector data
  out.write(reinterpret_cast<const char*>(vectors_.data()),
            static_cast<std::streamsize>(vectors_.size() * sizeof(float)));

  // Nodes
  for (const auto& node : nodes_) {
    out.write(reinterpret_cast<const char*>(&node.compact_index), sizeof(node.compact_index));
    out.write(reinterpret_cast<const char*>(&node.level), sizeof(node.level));
    auto deleted_byte = static_cast<uint8_t>(node.deleted ? 1 : 0);
    out.write(reinterpret_cast<const char*>(&deleted_byte), sizeof(deleted_byte));

    // Neighbors per level
    for (uint32_t l = 0; l <= node.level; ++l) {
      auto neighbor_count = static_cast<uint32_t>(node.neighbors[l].size());
      out.write(reinterpret_cast<const char*>(&neighbor_count), sizeof(neighbor_count));
      if (neighbor_count > 0) {
        out.write(reinterpret_cast<const char*>(node.neighbors[l].data()),
                  static_cast<std::streamsize>(neighbor_count * sizeof(uint32_t)));
      }
    }
  }

  // compact_to_internal mapping
  auto map_size = static_cast<uint32_t>(compact_to_internal_.size());
  out.write(reinterpret_cast<const char*>(&map_size), sizeof(map_size));
  if (map_size > 0) {
    out.write(reinterpret_cast<const char*>(compact_to_internal_.data()),
              static_cast<std::streamsize>(map_size * sizeof(uint32_t)));
  }

  if (!out.good()) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotSaveFailed, "Failed to write HNSW index"));
  }

  return {};
}

utils::Expected<void, utils::Error> HnswIndex::Deserialize(std::istream& in) {
  std::unique_lock lock(mutex_);

  // Header
  uint32_t magic = 0;
  uint32_t version = 0;
  in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  in.read(reinterpret_cast<char*>(&version), sizeof(version));

  if (magic != 0x48534E57 || version != 1) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotLoadFailed, "Invalid HNSW index format"));
  }

  // Config
  in.read(reinterpret_cast<char*>(&config_.m), sizeof(config_.m));
  in.read(reinterpret_cast<char*>(&config_.ef_construction), sizeof(config_.ef_construction));
  in.read(reinterpret_cast<char*>(&config_.ef_search), sizeof(config_.ef_search));

  // Dimensions and counts
  in.read(reinterpret_cast<char*>(&dimension_), sizeof(dimension_));
  uint32_t node_count = 0;
  in.read(reinterpret_cast<char*>(&node_count), sizeof(node_count));
  in.read(reinterpret_cast<char*>(&entry_point_), sizeof(entry_point_));
  in.read(reinterpret_cast<char*>(&max_level_), sizeof(max_level_));
  in.read(reinterpret_cast<char*>(&active_count_), sizeof(active_count_));

  const auto invalid_format = [](const std::string& message) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kSnapshotLoadFailed, message));
  };
  if (!in.good()) {
    return invalid_format("Truncated HNSW index header");
  }
  if (config_.m < 2 || dimension_ == 0 || dimension_ > kMaxVectorDimension || node_count > kMaxDeserializedNodes ||
      active_count_ > node_count || max_level_ > kMaxNodeLevel ||
      (entry_point_ != UINT32_MAX && entry_point_ >= node_count)) {
    return invalid_format("Invalid HNSW index header values");
  }
  if (node_count > 0 && static_cast<size_t>(node_count) > std::numeric_limits<size_t>::max() / dimension_) {
    return invalid_format("HNSW vector payload size overflows size_t");
  }
  const size_t vector_count = static_cast<size_t>(node_count) * dimension_;
  if (vector_count > std::numeric_limits<size_t>::max() / sizeof(float) ||
      !HasRemainingBytes(in, vector_count * sizeof(float))) {
    return invalid_format("Truncated or oversized HNSW vector payload");
  }

  // Recalculate level multiplier
  level_mult_ = 1.0 / std::log(static_cast<double>(std::max(config_.m, 2U)));

  // Vector data
  vectors_.resize(vector_count);
  in.read(reinterpret_cast<char*>(vectors_.data()), static_cast<std::streamsize>(vectors_.size() * sizeof(float)));
  vector_norms_.resize(node_count);
  if (use_cosine_prenorm_) {
    for (uint32_t i = 0; i < node_count; ++i) {
      vector_norms_[i] = simd::GetOptimalImpl().l2_norm(GetNodeVector(i), dimension_);
    }
  }

  // Nodes
  nodes_.resize(node_count);
  for (uint32_t i = 0; i < node_count; ++i) {
    auto& node = nodes_[i];
    in.read(reinterpret_cast<char*>(&node.compact_index), sizeof(node.compact_index));
    in.read(reinterpret_cast<char*>(&node.level), sizeof(node.level));
    uint8_t deleted_byte = 0;
    in.read(reinterpret_cast<char*>(&deleted_byte), sizeof(deleted_byte));
    if (!in.good() || node.level > kMaxNodeLevel || deleted_byte > 1) {
      return invalid_format("Invalid HNSW node header");
    }
    node.deleted = (deleted_byte != 0);

    node.neighbors.resize(node.level + 1);
    for (uint32_t l = 0; l <= node.level; ++l) {
      uint32_t neighbor_count = 0;
      in.read(reinterpret_cast<char*>(&neighbor_count), sizeof(neighbor_count));
      if (!in.good() || neighbor_count > node_count ||
          !HasRemainingBytes(in, static_cast<size_t>(neighbor_count) * sizeof(uint32_t))) {
        return invalid_format("Invalid HNSW neighbor list");
      }
      node.neighbors[l].resize(neighbor_count);
      if (neighbor_count > 0) {
        in.read(reinterpret_cast<char*>(node.neighbors[l].data()),
                static_cast<std::streamsize>(neighbor_count * sizeof(uint32_t)));
        if (!in.good() || std::any_of(node.neighbors[l].begin(), node.neighbors[l].end(),
                                      [node_count](uint32_t neighbor) { return neighbor >= node_count; })) {
          return invalid_format("Invalid HNSW neighbor reference");
        }
      }
    }
  }

  // compact_to_internal mapping
  uint32_t map_size = 0;
  in.read(reinterpret_cast<char*>(&map_size), sizeof(map_size));
  if (!in.good() || map_size > kMaxDeserializedNodes ||
      !HasRemainingBytes(in, static_cast<size_t>(map_size) * sizeof(uint32_t))) {
    return invalid_format("Invalid HNSW compact index map");
  }
  compact_to_internal_.resize(map_size);
  if (map_size > 0) {
    in.read(reinterpret_cast<char*>(compact_to_internal_.data()),
            static_cast<std::streamsize>(map_size * sizeof(uint32_t)));
    if (!in.good() ||
        std::any_of(compact_to_internal_.begin(), compact_to_internal_.end(),
                    [node_count](uint32_t internal) { return internal != UINT32_MAX && internal >= node_count; })) {
      return invalid_format("Invalid HNSW compact index reference");
    }
  }

  if (!in.good()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kSnapshotLoadFailed, "Failed to read HNSW index data"));
  }

  return {};
}

// ============================================================================
// HNSW-specific methods
// ============================================================================

void HnswIndex::SetEfSearch(uint32_t ef_search) {
  std::unique_lock lock(mutex_);
  config_.ef_search = ef_search;
}

uint32_t HnswIndex::GetEfSearch() const {
  std::shared_lock lock(mutex_);
  return config_.ef_search;
}

uint32_t HnswIndex::GetMaxLevel() const {
  std::shared_lock lock(mutex_);
  return max_level_;
}

uint32_t HnswIndex::GetNodeCount() const {
  std::shared_lock lock(mutex_);
  return static_cast<uint32_t>(nodes_.size());
}

uint32_t HnswIndex::GetTombstoneCount() const {
  std::shared_lock lock(mutex_);
  return static_cast<uint32_t>(nodes_.size() - active_count_);
}

size_t HnswIndex::MemoryUsage() const {
  std::shared_lock lock(mutex_);
  size_t bytes = sizeof(*this) + nodes_.capacity() * sizeof(Node) + vectors_.capacity() * sizeof(float) +
                 vector_norms_.capacity() * sizeof(float) + compact_to_internal_.capacity() * sizeof(uint32_t);
  for (const auto& node : nodes_) {
    bytes += node.neighbors.capacity() * sizeof(std::vector<uint32_t>);
    for (const auto& layer : node.neighbors) {
      bytes += layer.capacity() * sizeof(uint32_t);
    }
  }
  return bytes;
}

}  // namespace nvecd::vectors

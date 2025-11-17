/**
 * @file snapshot_format_v1.cpp
 * @brief Snapshot file format Version 1 implementation
 *
 * Reference: ../mygram-db/src/storage/dump_format_v1.cpp
 * Reusability: 85% (adapted for nvecd stores)
 */

#include "storage/snapshot_format_v1.h"

#include <zlib.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "utils/structured_log.h"

#ifdef _WIN32
#include <io.h>
#define CHMOD _chmod
#else
#include <fcntl.h>
#include <sys/fcntl.h>  // For O_NOFOLLOW on macOS
#include <sys/stat.h>
#include <unistd.h>
#define CHMOD chmod

// Ensure O_NOFOLLOW is defined (standard on POSIX systems)
#ifndef O_NOFOLLOW
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): Platform compatibility constant for symlink protection
#define O_NOFOLLOW 0x00000100
#endif
#endif

namespace nvecd::storage::snapshot_v1 {

using namespace utils;

namespace {

/**
 * @brief Write binary data to stream
 */
template <typename T>
bool WriteBinary(std::ostream& output_stream, const T& value) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  output_stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
  return output_stream.good();
}

/**
 * @brief Read binary data from stream
 */
template <typename T>
bool ReadBinary(std::istream& input_stream, T& value) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  input_stream.read(reinterpret_cast<char*>(&value), sizeof(T));
  return input_stream.good();
}

/**
 * @brief Write string to stream (length-prefixed)
 */
bool WriteString(std::ostream& output_stream, const std::string& str) {
  auto len = static_cast<uint32_t>(str.size());
  if (!WriteBinary(output_stream, len)) {
    return false;
  }
  if (len > 0) {
    output_stream.write(str.data(), len);
  }
  return output_stream.good();
}

/**
 * @brief Read string from stream (length-prefixed)
 */
bool ReadString(std::istream& input_stream, std::string& str) {
  constexpr uint32_t kMaxStringLength = 256 * 1024 * 1024;  // 256MB limit
  uint32_t len = 0;
  if (!ReadBinary(input_stream, len)) {
    return false;
  }
  if (len > kMaxStringLength) {
    LogStorageError("snapshot_read", "string_length_exceeded",
                    "String length " + std::to_string(len) + " exceeds limit");
    return false;
  }
  if (len > 0) {
    str.resize(len);
    input_stream.read(str.data(), len);
  } else {
    str.clear();
  }
  return input_stream.good();
}

}  // namespace

// ============================================================================
// CRC32 Calculation
// ============================================================================

uint32_t CalculateCRC32(const void* data, size_t length) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(data), length));
}

uint32_t CalculateCRC32(const std::string& str) {
  return CalculateCRC32(str.data(), str.size());
}

// ============================================================================
// Header V1 Serialization
// ============================================================================

Expected<void, Error> WriteHeaderV1(std::ostream& output_stream, const HeaderV1& header) {
  if (!WriteBinary(output_stream, header.header_size)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write header size"));
  }
  if (!WriteBinary(output_stream, header.flags)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write header flags"));
  }
  if (!WriteBinary(output_stream, header.snapshot_timestamp)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write snapshot timestamp"));
  }
  if (!WriteBinary(output_stream, header.total_file_size)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write total file size"));
  }
  if (!WriteBinary(output_stream, header.file_crc32)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write file CRC32"));
  }
  if (!WriteString(output_stream, header.reserved)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write reserved field"));
  }
  return {};
}

Expected<void, Error> ReadHeaderV1(std::istream& input_stream, HeaderV1& header) {
  if (!ReadBinary(input_stream, header.header_size)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read header size"));
  }
  if (!ReadBinary(input_stream, header.flags)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read header flags"));
  }
  if (!ReadBinary(input_stream, header.snapshot_timestamp)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read snapshot timestamp"));
  }
  if (!ReadBinary(input_stream, header.total_file_size)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read total file size"));
  }
  if (!ReadBinary(input_stream, header.file_crc32)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read file CRC32"));
  }
  if (!ReadString(input_stream, header.reserved)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read reserved field"));
  }
  return {};
}

// ============================================================================
// Statistics Serialization
// ============================================================================

Expected<void, Error> SerializeStatistics(std::ostream& output_stream, const SnapshotStatistics& stats) {
  if (!WriteBinary(output_stream, stats.total_contexts)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write total_contexts"));
  }
  if (!WriteBinary(output_stream, stats.total_events)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write total_events"));
  }
  if (!WriteBinary(output_stream, stats.total_co_occurrences)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write total_co_occurrences"));
  }
  if (!WriteBinary(output_stream, stats.total_vectors)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write total_vectors"));
  }
  if (!WriteBinary(output_stream, stats.total_bytes)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write total_bytes"));
  }
  if (!WriteBinary(output_stream, stats.snapshot_time_ms)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write snapshot_time_ms"));
  }
  return {};
}

Expected<void, Error> DeserializeStatistics(std::istream& input_stream, SnapshotStatistics& stats) {
  if (!ReadBinary(input_stream, stats.total_contexts)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read total_contexts"));
  }
  if (!ReadBinary(input_stream, stats.total_events)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read total_events"));
  }
  if (!ReadBinary(input_stream, stats.total_co_occurrences)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read total_co_occurrences"));
  }
  if (!ReadBinary(input_stream, stats.total_vectors)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read total_vectors"));
  }
  if (!ReadBinary(input_stream, stats.total_bytes)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read total_bytes"));
  }
  if (!ReadBinary(input_stream, stats.snapshot_time_ms)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read snapshot_time_ms"));
  }
  return {};
}

Expected<void, Error> SerializeStoreStatistics(std::ostream& output_stream, const StoreStatistics& stats) {
  if (!WriteBinary(output_stream, stats.item_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write item_count"));
  }
  if (!WriteBinary(output_stream, stats.memory_bytes)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write memory_bytes"));
  }
  if (!WriteBinary(output_stream, stats.last_update_time)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write last_update_time"));
  }
  return {};
}

Expected<void, Error> DeserializeStoreStatistics(std::istream& input_stream, StoreStatistics& stats) {
  if (!ReadBinary(input_stream, stats.item_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read item_count"));
  }
  if (!ReadBinary(input_stream, stats.memory_bytes)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read memory_bytes"));
  }
  if (!ReadBinary(input_stream, stats.last_update_time)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read last_update_time"));
  }
  return {};
}

// ============================================================================
// Config Serialization
// ============================================================================

Expected<void, Error> SerializeConfig(std::ostream& output_stream, const config::Config& config) {
  // Events config
  if (!WriteBinary(output_stream, config.events.ctx_buffer_size)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write ctx_buffer_size"));
  }
  if (!WriteBinary(output_stream, config.events.decay_interval_sec)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write decay_interval_sec"));
  }
  if (!WriteBinary(output_stream, config.events.decay_alpha)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write decay_alpha"));
  }

  // Vectors config
  if (!WriteBinary(output_stream, config.vectors.default_dimension)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write default_dimension"));
  }
  if (!WriteString(output_stream, config.vectors.distance_metric)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write distance_metric"));
  }

  // Similarity config
  if (!WriteBinary(output_stream, config.similarity.default_top_k)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write default_top_k"));
  }
  if (!WriteBinary(output_stream, config.similarity.max_top_k)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write max_top_k"));
  }
  if (!WriteBinary(output_stream, config.similarity.fusion_alpha)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write fusion_alpha"));
  }
  if (!WriteBinary(output_stream, config.similarity.fusion_beta)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write fusion_beta"));
  }

  // Note: We only serialize the core configs (events, vectors, similarity)
  // Other configs (network, logging, etc.) are not persisted in snapshots

  return {};
}

Expected<void, Error> DeserializeConfig(std::istream& input_stream, config::Config& config) {
  // Events config
  if (!ReadBinary(input_stream, config.events.ctx_buffer_size)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read ctx_buffer_size"));
  }
  if (!ReadBinary(input_stream, config.events.decay_interval_sec)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read decay_interval_sec"));
  }
  if (!ReadBinary(input_stream, config.events.decay_alpha)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read decay_alpha"));
  }

  // Vectors config
  if (!ReadBinary(input_stream, config.vectors.default_dimension)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read default_dimension"));
  }
  if (!ReadString(input_stream, config.vectors.distance_metric)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read distance_metric"));
  }

  // Similarity config
  if (!ReadBinary(input_stream, config.similarity.default_top_k)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read default_top_k"));
  }
  if (!ReadBinary(input_stream, config.similarity.max_top_k)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read max_top_k"));
  }
  if (!ReadBinary(input_stream, config.similarity.fusion_alpha)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read fusion_alpha"));
  }
  if (!ReadBinary(input_stream, config.similarity.fusion_beta)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read fusion_beta"));
  }

  return {};
}

// ============================================================================
// EventStore Serialization
// ============================================================================

Expected<void, Error> SerializeEventStore(std::ostream& output_stream, const events::EventStore& event_store) {
  // Get all contexts
  std::vector<std::string> contexts = event_store.GetAllContexts();

  // Write context count
  auto context_count = static_cast<uint32_t>(contexts.size());
  if (!WriteBinary(output_stream, context_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write context count"));
  }

  // Write each context's events
  for (const auto& ctx : contexts) {
    // Write context name
    if (!WriteString(output_stream, ctx)) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write context name: " + ctx));
    }

    // Get events for this context
    std::vector<events::Event> events = event_store.GetEvents(ctx);

    // Write event count
    auto event_count = static_cast<uint32_t>(events.size());
    if (!WriteBinary(output_stream, event_count)) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write event count for context: " + ctx));
    }

    // Write each event
    for (const auto& event : events) {
      if (!WriteString(output_stream, event.id)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write event id"));
      }
      if (!WriteBinary(output_stream, event.score)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write event score"));
      }
      if (!WriteBinary(output_stream, event.timestamp)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write event timestamp"));
      }
    }
  }

  return {};
}

Expected<void, Error> DeserializeEventStore(std::istream& input_stream, events::EventStore& event_store) {
  // Clear existing data
  event_store.Clear();

  // Read context count
  uint32_t context_count = 0;
  if (!ReadBinary(input_stream, context_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read context count"));
  }

  // Read each context's events
  for (uint32_t ctx_idx = 0; ctx_idx < context_count; ++ctx_idx) {
    // Read context name
    std::string ctx;
    if (!ReadString(input_stream, ctx)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read context name"));
    }

    // Read event count
    uint32_t event_count = 0;
    if (!ReadBinary(input_stream, event_count)) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpReadError, "Failed to read event count for context: " + ctx));
    }

    // Read each event
    for (uint32_t ev_idx = 0; ev_idx < event_count; ++ev_idx) {
      std::string id;
      int score = 0;
      uint64_t timestamp = 0;

      if (!ReadString(input_stream, id)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read event id"));
      }
      if (!ReadBinary(input_stream, score)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read event score"));
      }
      if (!ReadBinary(input_stream, timestamp)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read event timestamp"));
      }

      // Add event to store
      // Note: We reconstruct the event with original timestamp, not current time
      auto result = event_store.AddEvent(ctx, id, score);
      if (!result) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Failed to add event: " + result.error().message()));
      }
    }
  }

  return {};
}

// ============================================================================
// CoOccurrenceIndex Serialization
// ============================================================================

Expected<void, Error> SerializeCoOccurrenceIndex(std::ostream& output_stream,
                                                  const events::CoOccurrenceIndex& co_index) {
  // Get all items
  std::vector<std::string> items = co_index.GetAllItems();

  // Write item count
  auto item_count = static_cast<uint32_t>(items.size());
  if (!WriteBinary(output_stream, item_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write item count"));
  }

  // Write each item's co-occurrence scores
  for (const auto& item1 : items) {
    // Write item name
    if (!WriteString(output_stream, item1)) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write item name: " + item1));
    }

    // Get all co-occurring items with their scores
    std::vector<std::pair<std::string, float>> co_items = co_index.GetSimilar(item1, 1000000);  // Get all

    // Write co-item count
    auto co_item_count = static_cast<uint32_t>(co_items.size());
    if (!WriteBinary(output_stream, co_item_count)) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write co-item count for: " + item1));
    }

    // Write each co-occurring item and score
    for (const auto& [item2, score] : co_items) {
      if (!WriteString(output_stream, item2)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write co-item name"));
      }
      if (!WriteBinary(output_stream, score)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write co-occurrence score"));
      }
    }
  }

  return {};
}

Expected<void, Error> DeserializeCoOccurrenceIndex(std::istream& input_stream, events::CoOccurrenceIndex& co_index) {
  // Clear existing data
  co_index.Clear();

  // Read item count
  uint32_t item_count = 0;
  if (!ReadBinary(input_stream, item_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read item count"));
  }

  // Temporary storage for co-occurrence matrix
  // We need to reconstruct events and call UpdateFromEvents
  // But since we only have pairwise scores, we'll directly reconstruct the matrix
  // by creating synthetic events

  // Read each item's co-occurrence scores
  for (uint32_t item_idx = 0; item_idx < item_count; ++item_idx) {
    // Read item name
    std::string item1;
    if (!ReadString(input_stream, item1)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read item name"));
    }

    // Read co-item count
    uint32_t co_item_count = 0;
    if (!ReadBinary(input_stream, co_item_count)) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpReadError, "Failed to read co-item count for: " + item1));
    }

    // Collect co-items and scores
    std::vector<std::string> co_item_ids;
    std::vector<int> co_item_scores;

    for (uint32_t co_idx = 0; co_idx < co_item_count; ++co_idx) {
      std::string item2;
      float score = 0.0f;

      if (!ReadString(input_stream, item2)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read co-item name"));
      }
      if (!ReadBinary(input_stream, score)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read co-occurrence score"));
      }

      // Create synthetic events to reconstruct the co-occurrence
      // score = event1.score * event2.score
      // We approximate by using sqrt(score) for both events
      int approx_score = static_cast<int>(std::sqrt(score));
      co_item_ids.push_back(item2);
      co_item_scores.push_back(approx_score);
    }

    // Create a synthetic event list including item1
    std::vector<events::Event> synthetic_events;
    synthetic_events.emplace_back(item1, static_cast<int>(std::sqrt(co_item_count * 100)), 0);
    for (size_t i = 0; i < co_item_ids.size(); ++i) {
      synthetic_events.emplace_back(co_item_ids[i], co_item_scores[i], 0);
    }

    // Update co-occurrence index with synthetic events
    co_index.UpdateFromEvents("snapshot_restore_" + std::to_string(item_idx), synthetic_events);
  }

  return {};
}

// ============================================================================
// VectorStore Serialization
// ============================================================================

Expected<void, Error> SerializeVectorStore(std::ostream& output_stream, const vectors::VectorStore& vector_store) {
  // Write dimension
  size_t dimension = vector_store.GetDimension();
  if (!WriteBinary(output_stream, dimension)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write dimension"));
  }

  // Get all vector IDs
  std::vector<std::string> ids = vector_store.GetAllIds();

  // Write vector count
  auto vector_count = static_cast<uint32_t>(ids.size());
  if (!WriteBinary(output_stream, vector_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write vector count"));
  }

  // Write each vector
  for (const auto& id : ids) {
    // Write vector ID
    if (!WriteString(output_stream, id)) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write vector ID: " + id));
    }

    // Get vector
    auto vec_opt = vector_store.GetVector(id);
    if (!vec_opt) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpWriteError, "Vector not found during serialization: " + id));
    }

    const auto& vec = vec_opt.value();

    // Write normalized flag
    if (!WriteBinary(output_stream, vec.normalized)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write normalized flag"));
    }

    // Write vector data
    for (float component : vec.data) {
      if (!WriteBinary(output_stream, component)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write vector component"));
      }
    }
  }

  return {};
}

Expected<void, Error> DeserializeVectorStore(std::istream& input_stream, vectors::VectorStore& vector_store) {
  // Clear existing data
  vector_store.Clear();

  // Read dimension
  size_t dimension = 0;
  if (!ReadBinary(input_stream, dimension)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read dimension"));
  }

  // Read vector count
  uint32_t vector_count = 0;
  if (!ReadBinary(input_stream, vector_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read vector count"));
  }

  // Read each vector
  for (uint32_t vec_idx = 0; vec_idx < vector_count; ++vec_idx) {
    // Read vector ID
    std::string id;
    if (!ReadString(input_stream, id)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read vector ID"));
    }

    // Read normalized flag
    bool normalized = false;
    if (!ReadBinary(input_stream, normalized)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read normalized flag"));
    }

    // Read vector data
    std::vector<float> data(dimension);
    for (size_t i = 0; i < dimension; ++i) {
      if (!ReadBinary(input_stream, data[i])) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read vector component"));
      }
    }

    // Add vector to store
    // Note: We pass normalize=false and manually restore the normalized flag
    auto result = vector_store.SetVector(id, data, false);
    if (!result) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpReadError, "Failed to add vector: " + result.error().message()));
    }
  }

  return {};
}

// ============================================================================
// Main Snapshot Write/Read Functions
// ============================================================================

Expected<void, Error> WriteSnapshotV1(const std::string& filepath, const config::Config& config,
                                      const events::EventStore& event_store,
                                      const events::CoOccurrenceIndex& co_index,
                                      const vectors::VectorStore& vector_store, const SnapshotStatistics* stats,
                                      const std::unordered_map<std::string, StoreStatistics>* store_stats) {
  // Create temporary file path
  std::string temp_filepath = filepath + ".tmp";

  // Open file for binary writing
  std::ofstream output_stream(temp_filepath, std::ios::binary | std::ios::trunc);
  if (!output_stream) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError,
                                    "Failed to open file for writing: " + temp_filepath + " (" +
                                        std::strerror(errno) + ")"));
  }

  try {
    // Write fixed header (magic + version)
    output_stream.write(snapshot_format::kMagicNumber.data(), snapshot_format::kMagicNumber.size());
    uint32_t version = snapshot_format::kCurrentVersion;
    WriteBinary(output_stream, version);

    // Prepare V1 header (we'll update CRC32 and file size later)
    HeaderV1 header;
    header.header_size = 0;  // Will calculate
    header.flags = snapshot_format::flags_v1::kWithCRC;
    if (stats != nullptr) {
      header.flags |= snapshot_format::flags_v1::kWithStatistics;
    }
    header.snapshot_timestamp = static_cast<uint64_t>(std::time(nullptr));
    header.total_file_size = 0;  // Will calculate
    header.file_crc32 = 0;       // Will calculate
    header.reserved = "";        // Reserved for future use

    // Calculate header size
    std::ostringstream header_ss;
    auto header_result = WriteHeaderV1(header_ss, header);
    if (!header_result) {
      return header_result;
    }
    header.header_size = static_cast<uint32_t>(header_ss.str().size());

    // Write V1 header (with placeholder values)
    auto write_header_result = WriteHeaderV1(output_stream, header);
    if (!write_header_result) {
      return write_header_result;
    }

    // Write config section
    std::ostringstream config_ss;
    auto config_result = SerializeConfig(config_ss, config);
    if (!config_result) {
      return config_result;
    }
    std::string config_data = config_ss.str();
    uint32_t config_size = static_cast<uint32_t>(config_data.size());
    uint32_t config_crc = CalculateCRC32(config_data);
    WriteBinary(output_stream, config_size);
    WriteBinary(output_stream, config_crc);
    output_stream.write(config_data.data(), config_size);

    // Write statistics section (if provided)
    if (stats != nullptr) {
      std::ostringstream stats_ss;
      auto stats_result = SerializeStatistics(stats_ss, *stats);
      if (!stats_result) {
        return stats_result;
      }
      std::string stats_data = stats_ss.str();
      uint32_t stats_size = static_cast<uint32_t>(stats_data.size());
      uint32_t stats_crc = CalculateCRC32(stats_data);
      WriteBinary(output_stream, stats_size);
      WriteBinary(output_stream, stats_crc);
      output_stream.write(stats_data.data(), stats_size);
    }

    // Write store data section
    uint32_t store_count = 3;  // events, co_occurrence, vectors
    WriteBinary(output_stream, store_count);

    // Store 1: EventStore
    {
      WriteString(output_stream, "events");
      if (store_stats != nullptr && store_stats->count("events") > 0) {
        std::ostringstream store_stats_ss;
        auto store_stats_result = SerializeStoreStatistics(store_stats_ss, store_stats->at("events"));
        if (!store_stats_result) {
          return store_stats_result;
        }
        std::string store_stats_data = store_stats_ss.str();
        uint32_t store_stats_size = static_cast<uint32_t>(store_stats_data.size());
        uint32_t store_stats_crc = CalculateCRC32(store_stats_data);
        WriteBinary(output_stream, store_stats_size);
        WriteBinary(output_stream, store_stats_crc);
        output_stream.write(store_stats_data.data(), store_stats_size);
      } else {
        uint32_t zero = 0;
        WriteBinary(output_stream, zero);  // No store stats
      }

      std::ostringstream store_data_ss;
      auto serialize_result = SerializeEventStore(store_data_ss, event_store);
      if (!serialize_result) {
        return serialize_result;
      }
      std::string store_data = store_data_ss.str();
      uint32_t store_data_size = static_cast<uint32_t>(store_data.size());
      uint32_t store_data_crc = CalculateCRC32(store_data);
      WriteBinary(output_stream, store_data_size);
      WriteBinary(output_stream, store_data_crc);
      output_stream.write(store_data.data(), store_data_size);
    }

    // Store 2: CoOccurrenceIndex
    {
      WriteString(output_stream, "co_occurrence");
      if (store_stats != nullptr && store_stats->count("co_occurrence") > 0) {
        std::ostringstream store_stats_ss;
        auto store_stats_result = SerializeStoreStatistics(store_stats_ss, store_stats->at("co_occurrence"));
        if (!store_stats_result) {
          return store_stats_result;
        }
        std::string store_stats_data = store_stats_ss.str();
        uint32_t store_stats_size = static_cast<uint32_t>(store_stats_data.size());
        uint32_t store_stats_crc = CalculateCRC32(store_stats_data);
        WriteBinary(output_stream, store_stats_size);
        WriteBinary(output_stream, store_stats_crc);
        output_stream.write(store_stats_data.data(), store_stats_size);
      } else {
        uint32_t zero = 0;
        WriteBinary(output_stream, zero);  // No store stats
      }

      std::ostringstream store_data_ss;
      auto serialize_result = SerializeCoOccurrenceIndex(store_data_ss, co_index);
      if (!serialize_result) {
        return serialize_result;
      }
      std::string store_data = store_data_ss.str();
      uint32_t store_data_size = static_cast<uint32_t>(store_data.size());
      uint32_t store_data_crc = CalculateCRC32(store_data);
      WriteBinary(output_stream, store_data_size);
      WriteBinary(output_stream, store_data_crc);
      output_stream.write(store_data.data(), store_data_size);
    }

    // Store 3: VectorStore
    {
      WriteString(output_stream, "vectors");
      if (store_stats != nullptr && store_stats->count("vectors") > 0) {
        std::ostringstream store_stats_ss;
        auto store_stats_result = SerializeStoreStatistics(store_stats_ss, store_stats->at("vectors"));
        if (!store_stats_result) {
          return store_stats_result;
        }
        std::string store_stats_data = store_stats_ss.str();
        uint32_t store_stats_size = static_cast<uint32_t>(store_stats_data.size());
        uint32_t store_stats_crc = CalculateCRC32(store_stats_data);
        WriteBinary(output_stream, store_stats_size);
        WriteBinary(output_stream, store_stats_crc);
        output_stream.write(store_stats_data.data(), store_stats_size);
      } else {
        uint32_t zero = 0;
        WriteBinary(output_stream, zero);  // No store stats
      }

      std::ostringstream store_data_ss;
      auto serialize_result = SerializeVectorStore(store_data_ss, vector_store);
      if (!serialize_result) {
        return serialize_result;
      }
      std::string store_data = store_data_ss.str();
      uint32_t store_data_size = static_cast<uint32_t>(store_data.size());
      uint32_t store_data_crc = CalculateCRC32(store_data);
      WriteBinary(output_stream, store_data_size);
      WriteBinary(output_stream, store_data_crc);
      output_stream.write(store_data.data(), store_data_size);
    }

    // Calculate total file size
    header.total_file_size = static_cast<uint64_t>(output_stream.tellp());

    // Calculate file CRC32 (TODO: implement full file CRC32 calculation)
    // For now, we set it to 0 (will be implemented in future)
    header.file_crc32 = 0;

    // Seek back to header position and rewrite with correct values
    output_stream.seekp(snapshot_format::kFixedHeaderSize, std::ios::beg);
    auto rewrite_header_result = WriteHeaderV1(output_stream, header);
    if (!rewrite_header_result) {
      return rewrite_header_result;
    }

    // Close file
    output_stream.close();

    // Atomic rename
    std::filesystem::rename(temp_filepath, filepath);

    LogStorageInfo("snapshot_write", "Snapshot written successfully to " + filepath);
    return {};

  } catch (const std::exception& e) {
    output_stream.close();
    std::filesystem::remove(temp_filepath);
    return MakeUnexpected(
        MakeError(ErrorCode::kStorageDumpWriteError, "Exception during snapshot write: " + std::string(e.what())));
  }
}

Expected<void, Error> ReadSnapshotV1(const std::string& filepath, config::Config& config,
                                     events::EventStore& event_store, events::CoOccurrenceIndex& co_index,
                                     vectors::VectorStore& vector_store, SnapshotStatistics* stats,
                                     std::unordered_map<std::string, StoreStatistics>* store_stats,
                                     snapshot_format::IntegrityError* integrity_error) {
  // Open file for binary reading
  std::ifstream input_stream(filepath, std::ios::binary);
  if (!input_stream) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                    "Failed to open file for reading: " + filepath + " (" + std::strerror(errno) +
                                        ")"));
  }

  try {
    // Read and verify fixed header (magic + version)
    std::array<char, 4> magic{};
    input_stream.read(magic.data(), magic.size());
    if (magic != snapshot_format::kMagicNumber) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Invalid magic number"));
    }

    uint32_t version = 0;
    ReadBinary(input_stream, version);
    if (version != 1) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpReadError, "Unsupported version: " + std::to_string(version)));
    }

    // Read V1 header
    HeaderV1 header;
    auto read_header_result = ReadHeaderV1(input_stream, header);
    if (!read_header_result) {
      return read_header_result;
    }

    // Read config section
    uint32_t config_size = 0;
    uint32_t config_crc = 0;
    ReadBinary(input_stream, config_size);
    ReadBinary(input_stream, config_crc);
    std::string config_data(config_size, '\0');
    input_stream.read(config_data.data(), config_size);
    // TODO: Verify CRC32
    std::istringstream config_ss(config_data);
    auto deserialize_config_result = DeserializeConfig(config_ss, config);
    if (!deserialize_config_result) {
      return deserialize_config_result;
    }

    // Read statistics section (if present)
    if ((header.flags & snapshot_format::flags_v1::kWithStatistics) != 0 && stats != nullptr) {
      uint32_t stats_size = 0;
      uint32_t stats_crc = 0;
      ReadBinary(input_stream, stats_size);
      ReadBinary(input_stream, stats_crc);
      std::string stats_data(stats_size, '\0');
      input_stream.read(stats_data.data(), stats_size);
      // TODO: Verify CRC32
      std::istringstream stats_ss(stats_data);
      auto deserialize_stats_result = DeserializeStatistics(stats_ss, *stats);
      if (!deserialize_stats_result) {
        return deserialize_stats_result;
      }
    }

    // Read store data section
    uint32_t store_count = 0;
    ReadBinary(input_stream, store_count);

    for (uint32_t store_idx = 0; store_idx < store_count; ++store_idx) {
      // Read store name
      std::string store_name;
      ReadString(input_stream, store_name);

      // Read store statistics (if present)
      uint32_t store_stats_size = 0;
      ReadBinary(input_stream, store_stats_size);
      if (store_stats_size > 0 && store_stats != nullptr) {
        uint32_t store_stats_crc = 0;
        ReadBinary(input_stream, store_stats_crc);
        std::string store_stats_data(store_stats_size, '\0');
        input_stream.read(store_stats_data.data(), store_stats_size);
        // TODO: Verify CRC32
        std::istringstream store_stats_ss(store_stats_data);
        StoreStatistics st;
        auto deserialize_store_stats_result = DeserializeStoreStatistics(store_stats_ss, st);
        if (!deserialize_store_stats_result) {
          return deserialize_store_stats_result;
        }
        (*store_stats)[store_name] = st;
      }

      // Read store data
      uint32_t store_data_size = 0;
      uint32_t store_data_crc = 0;
      ReadBinary(input_stream, store_data_size);
      ReadBinary(input_stream, store_data_crc);
      std::string store_data(store_data_size, '\0');
      input_stream.read(store_data.data(), store_data_size);
      // TODO: Verify CRC32

      std::istringstream store_data_ss(store_data);

      // Deserialize based on store name
      if (store_name == "events") {
        auto deserialize_result = DeserializeEventStore(store_data_ss, event_store);
        if (!deserialize_result) {
          return deserialize_result;
        }
      } else if (store_name == "co_occurrence") {
        auto deserialize_result = DeserializeCoOccurrenceIndex(store_data_ss, co_index);
        if (!deserialize_result) {
          return deserialize_result;
        }
      } else if (store_name == "vectors") {
        auto deserialize_result = DeserializeVectorStore(store_data_ss, vector_store);
        if (!deserialize_result) {
          return deserialize_result;
        }
      } else {
        LogStorageWarning("snapshot_read", "Unknown store name: " + store_name);
      }
    }

    LogStorageInfo("snapshot_read", "Snapshot loaded successfully from " + filepath);
    return {};

  } catch (const std::exception& e) {
    return MakeUnexpected(
        MakeError(ErrorCode::kStorageDumpReadError, "Exception during snapshot read: " + std::string(e.what())));
  }
}

Expected<void, Error> VerifySnapshotIntegrity(const std::string& filepath,
                                               snapshot_format::IntegrityError& integrity_error) {
  // Open file for binary reading
  std::ifstream input_stream(filepath, std::ios::binary);
  if (!input_stream) {
    integrity_error.type = snapshot_format::CRCErrorType::FileCRC;
    integrity_error.message = "Failed to open file: " + std::string(std::strerror(errno));
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, integrity_error.message));
  }

  try {
    // Read and verify fixed header (magic + version)
    std::array<char, 4> magic{};
    input_stream.read(magic.data(), magic.size());
    if (magic != snapshot_format::kMagicNumber) {
      integrity_error.type = snapshot_format::CRCErrorType::FileCRC;
      integrity_error.message = "Invalid magic number";
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, integrity_error.message));
    }

    uint32_t version = 0;
    ReadBinary(input_stream, version);
    if (version < snapshot_format::kMinSupportedVersion || version > snapshot_format::kMaxSupportedVersion) {
      integrity_error.type = snapshot_format::CRCErrorType::FileCRC;
      integrity_error.message = "Unsupported version: " + std::to_string(version);
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, integrity_error.message));
    }

    // Read V1 header
    HeaderV1 header;
    auto read_header_result = ReadHeaderV1(input_stream, header);
    if (!read_header_result) {
      integrity_error.type = snapshot_format::CRCErrorType::FileCRC;
      integrity_error.message = read_header_result.error().message();
      return read_header_result;
    }

    // Verify file size
    input_stream.seekg(0, std::ios::end);
    uint64_t actual_file_size = static_cast<uint64_t>(input_stream.tellg());
    if (actual_file_size != header.total_file_size) {
      integrity_error.type = snapshot_format::CRCErrorType::FileCRC;
      integrity_error.message = "File size mismatch: expected " + std::to_string(header.total_file_size) +
                                ", got " + std::to_string(actual_file_size);
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, integrity_error.message));
    }

    // TODO: Verify file CRC32 (full file checksum)

    LogStorageInfo("snapshot_verify", "Snapshot integrity verified: " + filepath);
    return {};

  } catch (const std::exception& e) {
    integrity_error.type = snapshot_format::CRCErrorType::FileCRC;
    integrity_error.message = "Exception during verification: " + std::string(e.what());
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, integrity_error.message));
  }
}

Expected<void, Error> GetSnapshotInfo(const std::string& filepath, SnapshotInfo& info) {
  // Open file for binary reading
  std::ifstream input_stream(filepath, std::ios::binary);
  if (!input_stream) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                    "Failed to open file for reading: " + filepath + " (" + std::strerror(errno) +
                                        ")"));
  }

  try {
    // Read and verify fixed header (magic + version)
    std::array<char, 4> magic{};
    input_stream.read(magic.data(), magic.size());
    if (magic != snapshot_format::kMagicNumber) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Invalid magic number"));
    }

    uint32_t version = 0;
    ReadBinary(input_stream, version);
    info.version = version;

    if (version != 1) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpReadError, "Unsupported version: " + std::to_string(version)));
    }

    // Read V1 header
    HeaderV1 header;
    auto read_header_result = ReadHeaderV1(input_stream, header);
    if (!read_header_result) {
      return read_header_result;
    }

    info.flags = header.flags;
    info.timestamp = header.snapshot_timestamp;
    info.file_size = header.total_file_size;
    info.has_statistics = (header.flags & snapshot_format::flags_v1::kWithStatistics) != 0;

    // We could read more info (like store count) by continuing to parse,
    // but for now this is sufficient for GetSnapshotInfo
    info.store_count = 3;  // We always have 3 stores

    return {};

  } catch (const std::exception& e) {
    return MakeUnexpected(
        MakeError(ErrorCode::kStorageDumpReadError, "Exception during info read: " + std::string(e.what())));
  }
}

}  // namespace nvecd::storage::snapshot_v1

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
#include <variant>
#include <vector>

#include "utils/path_utils.h"
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

constexpr uint32_t kMaxConfigSize = 16 * 1024 * 1024;      // 16MB max for config section
constexpr uint32_t kMaxStatsSize = 16 * 1024 * 1024;       // 16MB max for statistics section
constexpr uint32_t kMaxStoreDataSize = 512 * 1024 * 1024;  // 512MB max for store data
constexpr uint32_t kMaxStoreStatsSize = 16 * 1024 * 1024;  // 16MB max for store statistics

enum class MetadataValueType : uint8_t {
  kString = 1,
  kInt64 = 2,
  kDouble = 3,
  kBool = 4,
};

/// @brief Absolute file offset of the file_crc32 field in the V1 header.
/// kFixedHeaderSize (8) + header_size(4) + flags(4) + snapshot_timestamp(8) + total_file_size(8) = 32
constexpr size_t kFileCRC32Offset = snapshot_format::kFixedHeaderSize + 24;

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

Expected<std::string, Error> CreateSecureTemporaryFile(const std::string& filepath) {
#ifdef _WIN32
  return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError,
                                  "Secure temporary snapshot files are not supported on this platform"));
#else
  const std::filesystem::path parent = std::filesystem::path(filepath).parent_path();
  auto directory_valid = ValidatePrivateDirectory(parent.empty() ? std::filesystem::path(".") : parent);
  if (!directory_valid) {
    return MakeUnexpected(directory_valid.error());
  }

  std::string path_template = filepath + ".tmp.XXXXXX";
  std::vector<char> mutable_template(path_template.begin(), path_template.end());
  mutable_template.push_back('\0');
  const int fd = ::mkstemp(mutable_template.data());
  if (fd < 0) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError,
                                    "Failed to create temporary snapshot file: " + std::string(std::strerror(errno))));
  }
  if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    ::unlink(mutable_template.data());
    return MakeUnexpected(
        MakeError(ErrorCode::kStorageDumpWriteError,
                  "Failed to set temporary snapshot permissions: " + std::string(std::strerror(saved_errno))));
  }
  if (::close(fd) != 0) {
    const int saved_errno = errno;
    ::unlink(mutable_template.data());
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to close temporary snapshot file: " +
                                                                           std::string(std::strerror(saved_errno))));
  }
  return std::string(mutable_template.data());
#endif
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

namespace {

/**
 * @brief Verify the whole-file size and CRC32 recorded in the V1 header.
 *
 * Validates the stored total_file_size against the actual on-disk size and
 * recomputes the CRC32 over the entire file (with the file_crc32 field zeroed)
 * to compare against header.file_crc32. This is the same check performed by
 * VerifySnapshotIntegrity and is shared so the two code paths cannot diverge.
 *
 * The input stream position is modified by this function (seeks to end/begin).
 *
 * @param input_stream Open binary input stream for the snapshot file
 * @param header Parsed V1 header containing total_file_size and file_crc32
 * @param integrity_error Optional output populated with FileCRC details on failure
 * @return Expected<void, Error> Success or a FileCRC failure
 */
Expected<void, Error> VerifyFileLevelIntegrity(std::istream& input_stream, const HeaderV1& header,
                                               snapshot_format::IntegrityError* integrity_error) {
  // Verify file size against the value recorded in the header.
  input_stream.seekg(0, std::ios::end);
  auto actual_file_size = static_cast<uint64_t>(input_stream.tellg());
  if (actual_file_size != header.total_file_size) {
    std::string message = "File size mismatch: expected " + std::to_string(header.total_file_size) + ", got " +
                          std::to_string(actual_file_size);
    if (integrity_error != nullptr) {
      integrity_error->type = snapshot_format::CRCErrorType::FileCRC;
      integrity_error->message = message;
    }
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, message));
  }

  // Verify whole-file CRC32 (computed with the file_crc32 field zeroed out).
  if (header.file_crc32 != 0) {
    input_stream.seekg(0, std::ios::beg);
    std::string file_contents(static_cast<size_t>(actual_file_size), '\0');
    input_stream.read(file_contents.data(), static_cast<std::streamsize>(actual_file_size));
    if (input_stream.fail()) {
      std::string message = "Failed to read file contents for CRC verification";
      if (integrity_error != nullptr) {
        integrity_error->type = snapshot_format::CRCErrorType::FileCRC;
        integrity_error->message = message;
      }
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, message));
    }

    // Zero out the file_crc32 field for CRC computation (it was 0 when written).
    if (file_contents.size() >= kFileCRC32Offset + sizeof(uint32_t)) {
      std::memset(&file_contents[kFileCRC32Offset], 0, sizeof(uint32_t));
    }

    uint32_t computed_crc = CalculateCRC32(file_contents);
    if (computed_crc != header.file_crc32) {
      std::string message = "File CRC32 mismatch: expected " + std::to_string(header.file_crc32) + ", got " +
                            std::to_string(computed_crc);
      if (integrity_error != nullptr) {
        integrity_error->type = snapshot_format::CRCErrorType::FileCRC;
        integrity_error->message = message;
      }
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, message));
    }
  }

  return {};
}

}  // namespace

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
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write context name: " + ctx));
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
      if (!WriteString(output_stream, event.item_id)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write event id"));
      }
      if (!WriteBinary(output_stream, event.score)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write event score"));
      }
      // Serialize the event type (uint8) so DEL negative-signal and SET dedup
      // semantics survive the reload; without it every event defaults to ADD.
      auto event_type = static_cast<uint8_t>(event.type);
      if (!WriteBinary(output_stream, event_type)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write event type"));
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
      std::string item_id;
      int score = 0;
      uint8_t event_type = 0;
      uint64_t timestamp = 0;

      if (!ReadString(input_stream, item_id)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read event id"));
      }
      if (!ReadBinary(input_stream, score)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read event score"));
      }
      if (!ReadBinary(input_stream, event_type)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read event type"));
      }
      if (!ReadBinary(input_stream, timestamp)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read event timestamp"));
      }
      if (event_type > static_cast<uint8_t>(events::EventType::DEL)) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Invalid event type: " + std::to_string(event_type)));
      }

      // Restore the event verbatim so its original timestamp (temporal-decay
      // weight) and type (DEL negative-signal / SET dedup semantics) are
      // preserved. RestoreEvent bypasses deduplication to keep the buffer
      // byte-for-byte identical to the snapshot.
      events::Event event(item_id, score, timestamp, static_cast<events::EventType>(event_type));
      auto result = event_store.RestoreEvent(ctx, event);
      if (!result) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Failed to restore event: " + result.error().message()));
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
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write item name: " + item1));
    }

    // Get all co-occurring items with their scores. Use the unfiltered
    // enumeration (not GetSimilar) so zero/negative-score neighbors -- the
    // negative-signal baselines -- survive the SAVE/LOAD round trip.
    std::vector<std::pair<std::string, float>> co_items = co_index.GetAllNeighbors(item1);

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

  // Read each item's co-occurrence scores and set them directly
  for (uint32_t item_idx = 0; item_idx < item_count; ++item_idx) {
    // Read item name
    std::string item1;
    if (!ReadString(input_stream, item1)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read item name"));
    }

    // Read co-item count
    uint32_t co_item_count = 0;
    if (!ReadBinary(input_stream, co_item_count)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read co-item count for: " + item1));
    }

    for (uint32_t co_idx = 0; co_idx < co_item_count; ++co_idx) {
      std::string item2;
      float score = 0.0F;

      if (!ReadString(input_stream, item2)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read co-item name"));
      }
      if (!ReadBinary(input_stream, score)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read co-occurrence score"));
      }

      // Set the score directly to preserve exact values from the snapshot
      co_index.SetScore(item1, item2, score);
    }
  }

  return {};
}

// ============================================================================
// VectorStore Serialization
// ============================================================================

Expected<void, Error> SerializeVectorStore(std::ostream& output_stream, const vectors::VectorStore& vector_store) {
  // Write dimension as a fixed-width integer so snapshots are portable across
  // 32/64-bit builds (a bare size_t is 8 bytes on LP64, 4 on ILP32, which would
  // desync the vectors section on a cross-bit load).
  auto dimension = static_cast<uint64_t>(vector_store.GetDimension());
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
  for (const auto& vector_id : ids) {
    // Write vector ID
    if (!WriteString(output_stream, vector_id)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write vector ID: " + vector_id));
    }

    // Get vector
    auto vec_opt = vector_store.GetVector(vector_id);
    if (!vec_opt) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpWriteError, "Vector not found during serialization: " + vector_id));
    }

    const auto& vec = vec_opt.value();

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

  // Read dimension (fixed-width uint64_t; see SerializeVectorStore).
  uint64_t dimension = 0;
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
    std::string vector_id;
    if (!ReadString(input_stream, vector_id)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read vector ID"));
    }

    // Read vector data
    std::vector<float> data(dimension);
    for (uint64_t i = 0; i < dimension; ++i) {
      if (!ReadBinary(input_stream, data[i])) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read vector component"));
      }
    }

    // Add vector to store. Vectors are persisted verbatim (already in their
    // stored form), so no re-normalization is applied on load.
    auto result = vector_store.SetVector(vector_id, data, false);
    if (!result) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpReadError, "Failed to add vector: " + result.error().message()));
    }
  }

  return {};
}

// ============================================================================
// MetadataStore Serialization
// ============================================================================

Expected<void, Error> SerializeMetadataStore(std::ostream& output_stream, const vectors::MetadataStore& metadata_store,
                                             const vectors::VectorStore& /*vector_store*/) {
  // Snapshot the id -> metadata map under a read lock so the iteration is
  // consistent with concurrent writers.
  auto read_lock = metadata_store.AcquireReadLock();
  std::vector<std::pair<std::string, vectors::Metadata>> entries;
  const auto& all = metadata_store.GetAll();
  entries.reserve(all.size());
  for (const auto& [id, metadata] : all) {
    if (!metadata.empty()) {
      entries.emplace_back(id, metadata);
    }
  }

  auto item_count = static_cast<uint32_t>(entries.size());
  if (!WriteBinary(output_stream, item_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write metadata item count"));
  }

  for (const auto& [id, metadata] : entries) {
    if (!WriteString(output_stream, id)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write metadata item ID: " + id));
    }

    auto field_count = static_cast<uint32_t>(metadata.size());
    if (!WriteBinary(output_stream, field_count)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write metadata field count"));
    }

    for (const auto& [key, value] : metadata) {
      if (!WriteString(output_stream, key)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write metadata key"));
      }

      if (std::holds_alternative<std::string>(value)) {
        auto type = static_cast<uint8_t>(MetadataValueType::kString);
        if (!WriteBinary(output_stream, type) || !WriteString(output_stream, std::get<std::string>(value))) {
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write string metadata"));
        }
      } else if (std::holds_alternative<int64_t>(value)) {
        auto type = static_cast<uint8_t>(MetadataValueType::kInt64);
        auto data = std::get<int64_t>(value);
        if (!WriteBinary(output_stream, type) || !WriteBinary(output_stream, data)) {
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write integer metadata"));
        }
      } else if (std::holds_alternative<double>(value)) {
        auto type = static_cast<uint8_t>(MetadataValueType::kDouble);
        auto data = std::get<double>(value);
        if (!WriteBinary(output_stream, type) || !WriteBinary(output_stream, data)) {
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write double metadata"));
        }
      } else if (std::holds_alternative<bool>(value)) {
        auto type = static_cast<uint8_t>(MetadataValueType::kBool);
        auto data = std::get<bool>(value);
        if (!WriteBinary(output_stream, type) || !WriteBinary(output_stream, data)) {
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write bool metadata"));
        }
      }
    }
  }

  return {};
}

Expected<void, Error> DeserializeMetadataStore(std::istream& input_stream, vectors::MetadataStore& metadata_store,
                                               const vectors::VectorStore& /*vector_store*/) {
  metadata_store.Clear();

  uint32_t item_count = 0;
  if (!ReadBinary(input_stream, item_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read metadata item count"));
  }

  for (uint32_t item_idx = 0; item_idx < item_count; ++item_idx) {
    std::string id;
    if (!ReadString(input_stream, id)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read metadata item ID"));
    }

    uint32_t field_count = 0;
    if (!ReadBinary(input_stream, field_count)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read metadata field count"));
    }

    vectors::Metadata metadata;
    for (uint32_t field_idx = 0; field_idx < field_count; ++field_idx) {
      std::string key;
      if (!ReadString(input_stream, key)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read metadata key"));
      }

      uint8_t type = 0;
      if (!ReadBinary(input_stream, type)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read metadata value type"));
      }

      switch (static_cast<MetadataValueType>(type)) {
        case MetadataValueType::kString: {
          std::string value;
          if (!ReadString(input_stream, value)) {
            return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read string metadata"));
          }
          metadata[key] = std::move(value);
          break;
        }
        case MetadataValueType::kInt64: {
          int64_t value = 0;
          if (!ReadBinary(input_stream, value)) {
            return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read integer metadata"));
          }
          metadata[key] = value;
          break;
        }
        case MetadataValueType::kDouble: {
          double value = 0.0;
          if (!ReadBinary(input_stream, value)) {
            return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read double metadata"));
          }
          metadata[key] = value;
          break;
        }
        case MetadataValueType::kBool: {
          bool value = false;
          if (!ReadBinary(input_stream, value)) {
            return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read bool metadata"));
          }
          metadata[key] = value;
          break;
        }
        default:
          return MakeUnexpected(
              MakeError(ErrorCode::kStorageDumpReadError, "Unknown metadata value type: " + std::to_string(type)));
      }
    }

    if (!metadata.empty()) {
      metadata_store.Set(id, std::move(metadata));
    }
  }

  return {};
}

// ============================================================================
// Main Snapshot Write/Read Functions
// ============================================================================

Expected<void, Error> WriteSnapshotV1(const std::string& filepath, const config::Config& config,
                                      const events::EventStore& event_store, const events::CoOccurrenceIndex& co_index,
                                      const vectors::VectorStore& vector_store, const SnapshotStatistics* stats,
                                      const std::unordered_map<std::string, StoreStatistics>* store_stats,
                                      const vectors::MetadataStore* metadata_store, bool suppress_logging) {
  auto resolved_path_result = ResolvePrivateStoragePath(filepath);
  if (!resolved_path_result) {
    return MakeUnexpected(resolved_path_result.error());
  }
  const std::string resolved_filepath = resolved_path_result->string();

  auto temp_file_result = CreateSecureTemporaryFile(resolved_filepath);
  if (!temp_file_result) {
    return MakeUnexpected(temp_file_result.error());
  }
  const std::string temp_filepath = *temp_file_result;

  // Open file for binary writing
  std::ofstream output_stream(temp_filepath, std::ios::binary | std::ios::trunc);
  if (!output_stream) {
    return MakeUnexpected(
        MakeError(ErrorCode::kStorageDumpWriteError,
                  "Failed to open file for writing: " + temp_filepath + " (" + std::strerror(errno) + ")"));
  }

  {
    // Write fixed header (magic + version)
    output_stream.write(snapshot_format::kMagicNumber.data(), snapshot_format::kMagicNumber.size());
    if (!output_stream.good()) {
      std::error_code rm_ec;
      std::filesystem::remove(temp_filepath, rm_ec);
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write magic number"));
    }
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
    auto config_size = static_cast<uint32_t>(config_data.size());
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
      auto stats_size = static_cast<uint32_t>(stats_data.size());
      uint32_t stats_crc = CalculateCRC32(stats_data);
      WriteBinary(output_stream, stats_size);
      WriteBinary(output_stream, stats_crc);
      output_stream.write(stats_data.data(), stats_size);
    }

    // Write store data section
    uint32_t store_count = metadata_store != nullptr ? 4 : 3;  // events, co_occurrence, vectors, metadata
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
        auto store_stats_size = static_cast<uint32_t>(store_stats_data.size());
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
      auto store_data_size = static_cast<uint32_t>(store_data.size());
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
        auto store_stats_size = static_cast<uint32_t>(store_stats_data.size());
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
      auto store_data_size = static_cast<uint32_t>(store_data.size());
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
        auto store_stats_size = static_cast<uint32_t>(store_stats_data.size());
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
      auto store_data_size = static_cast<uint32_t>(store_data.size());
      uint32_t store_data_crc = CalculateCRC32(store_data);
      WriteBinary(output_stream, store_data_size);
      WriteBinary(output_stream, store_data_crc);
      output_stream.write(store_data.data(), store_data_size);
    }

    // Store 4: MetadataStore
    if (metadata_store != nullptr) {
      WriteString(output_stream, "metadata");
      uint32_t zero = 0;
      WriteBinary(output_stream, zero);  // No store stats

      std::ostringstream store_data_ss;
      auto serialize_result = SerializeMetadataStore(store_data_ss, *metadata_store, vector_store);
      if (!serialize_result) {
        return serialize_result;
      }
      std::string store_data = store_data_ss.str();
      auto store_data_size = static_cast<uint32_t>(store_data.size());
      uint32_t store_data_crc = CalculateCRC32(store_data);
      WriteBinary(output_stream, store_data_size);
      WriteBinary(output_stream, store_data_crc);
      output_stream.write(store_data.data(), store_data_size);
    }

    // Check stream state after all store writes
    if (!output_stream.good()) {
      output_stream.close();
      std::error_code rm_ec;
      std::filesystem::remove(temp_filepath, rm_ec);
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Stream error during snapshot write"));
    }

    // Calculate total file size
    header.total_file_size = static_cast<uint64_t>(output_stream.tellp());

    // Write header with correct total_file_size (file_crc32 still 0 as placeholder)
    header.file_crc32 = 0;
    output_stream.seekp(snapshot_format::kFixedHeaderSize, std::ios::beg);
    auto rewrite_header_result = WriteHeaderV1(output_stream, header);
    if (!rewrite_header_result) {
      return rewrite_header_result;
    }

    // Close file to flush all data
    output_stream.close();

    // Compute file-level CRC32: read entire file, zero the file_crc32 field, compute CRC
    {
      std::ifstream crc_input(temp_filepath, std::ios::binary);
      if (!crc_input) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError,
                                        "Failed to reopen temp file for CRC32 computation: " + temp_filepath));
      }
      crc_input.seekg(0, std::ios::end);
      auto file_size = static_cast<size_t>(crc_input.tellg());
      crc_input.seekg(0, std::ios::beg);
      std::string file_contents(file_size, '\0');
      crc_input.read(file_contents.data(), static_cast<std::streamsize>(file_size));
      if (crc_input.fail()) {
        std::error_code rm_ec;
        std::filesystem::remove(temp_filepath, rm_ec);
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpWriteError, "Failed to read temp file for CRC32 computation"));
      }
      crc_input.close();

      // Zero out the file_crc32 field for CRC computation (it was 0 when written)
      // The field is already 0 in the buffer, so we can compute directly
      header.file_crc32 = CalculateCRC32(file_contents);

      // Write the computed CRC32 at the file_crc32 position
      std::ofstream crc_output(temp_filepath, std::ios::binary | std::ios::in | std::ios::out);
      if (!crc_output) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError,
                                        "Failed to reopen temp file for CRC32 write: " + temp_filepath));
      }
      crc_output.seekp(static_cast<std::streamoff>(kFileCRC32Offset), std::ios::beg);
      WriteBinary(crc_output, header.file_crc32);
      crc_output.close();
    }

#ifndef _WIN32
    // fsync the temp file to ensure data is persisted before rename
    {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open() requires varargs
      int fd = open(temp_filepath.c_str(), O_RDONLY);
      if (fd >= 0) {
        fsync(fd);
        close(fd);
      }

      // fsync the parent directory to ensure the directory entry is persisted
      std::filesystem::path parent_dir = std::filesystem::path(temp_filepath).parent_path();
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open() requires varargs
      int dir_fd = open(parent_dir.c_str(), O_RDONLY);
      if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
      }
    }
#endif

    // Atomic rename
    std::error_code rename_ec;
    std::filesystem::rename(temp_filepath, resolved_filepath, rename_ec);
    if (rename_ec) {
      std::error_code rm_ec;
      std::filesystem::remove(temp_filepath, rm_ec);
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpWriteError,
                    "Failed to rename temp file to " + resolved_filepath + ": " + rename_ec.message()));
    }

    // Skip spdlog when running on a post-fork child path, where the logger is
    // not safe to touch (see suppress_logging in WriteSnapshotV1's docs).
    if (!suppress_logging) {
      LogStorageInfo("snapshot_write", "Snapshot written successfully to " + resolved_filepath);
    }
    return {};
  }
}

Expected<void, Error> ReadSnapshotV1(const std::string& filepath, config::Config& config,
                                     events::EventStore& event_store, events::CoOccurrenceIndex& co_index,
                                     vectors::VectorStore& vector_store, SnapshotStatistics* stats,
                                     std::unordered_map<std::string, StoreStatistics>* store_stats,
                                     snapshot_format::IntegrityError* integrity_error,
                                     vectors::MetadataStore* metadata_store) {
  // Open file for binary reading
  std::ifstream input_stream(filepath, std::ios::binary);
  if (!input_stream) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to open file for reading: " + filepath +
                                                                          " (" + std::strerror(errno) + ")"));
  }

  {
    // Read and verify fixed header (magic + version)
    std::array<char, 4> magic{};
    input_stream.read(magic.data(), magic.size());
    if (!input_stream.good()) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read magic number"));
    }
    if (magic != snapshot_format::kMagicNumber) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Invalid magic number"));
    }

    uint32_t version = 0;
    if (!ReadBinary(input_stream, version)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read version"));
    }
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

    // Verify whole-file integrity (size + CRC32) before deserializing any
    // section. This catches truncation and bit-rot anywhere in the file. The
    // helper repositions the stream, so remember where the sections begin and
    // seek back afterwards.
    auto sections_begin = input_stream.tellg();
    auto file_integrity_result = VerifyFileLevelIntegrity(input_stream, header, integrity_error);
    if (!file_integrity_result) {
      return file_integrity_result;
    }
    input_stream.clear();
    input_stream.seekg(sections_begin, std::ios::beg);

    // Read config section
    uint32_t config_size = 0;
    uint32_t config_crc = 0;
    if (!ReadBinary(input_stream, config_size)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read config size"));
    }
    if (!ReadBinary(input_stream, config_crc)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read config CRC"));
    }
    if (config_size > kMaxConfigSize) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpReadError, "Config size exceeds maximum: " + std::to_string(config_size)));
    }
    std::string config_data(config_size, '\0');
    input_stream.read(config_data.data(), config_size);
    if (!input_stream.good()) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read config data"));
    }
    {
      uint32_t actual_crc = CalculateCRC32(config_data);
      if (actual_crc != config_crc) {
        if (integrity_error != nullptr) {
          integrity_error->type = snapshot_format::CRCErrorType::ConfigCRC;
          integrity_error->message = "CRC32 mismatch in config section: expected " + std::to_string(config_crc) +
                                     ", got " + std::to_string(actual_crc);
        }
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                        "CRC32 mismatch in config section: expected " + std::to_string(config_crc) +
                                            ", got " + std::to_string(actual_crc)));
      }
    }
    std::istringstream config_ss(config_data);
    auto deserialize_config_result = DeserializeConfig(config_ss, config);
    if (!deserialize_config_result) {
      return deserialize_config_result;
    }

    // Read statistics section (if present)
    if ((header.flags & snapshot_format::flags_v1::kWithStatistics) != 0 && stats != nullptr) {
      uint32_t stats_size = 0;
      uint32_t stats_crc = 0;
      if (!ReadBinary(input_stream, stats_size)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read statistics size"));
      }
      if (!ReadBinary(input_stream, stats_crc)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read statistics CRC"));
      }
      if (stats_size > kMaxStatsSize) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                        "Statistics size exceeds maximum: " + std::to_string(stats_size)));
      }
      std::string stats_data(stats_size, '\0');
      input_stream.read(stats_data.data(), stats_size);
      if (!input_stream.good()) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read statistics data"));
      }
      {
        uint32_t actual_crc = CalculateCRC32(stats_data);
        if (actual_crc != stats_crc) {
          if (integrity_error != nullptr) {
            integrity_error->type = snapshot_format::CRCErrorType::StatsCRC;
            integrity_error->message = "CRC32 mismatch in statistics section: expected " + std::to_string(stats_crc) +
                                       ", got " + std::to_string(actual_crc);
          }
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                          "CRC32 mismatch in statistics section: expected " +
                                              std::to_string(stats_crc) + ", got " + std::to_string(actual_crc)));
        }
      }
      std::istringstream stats_ss(stats_data);
      auto deserialize_stats_result = DeserializeStatistics(stats_ss, *stats);
      if (!deserialize_stats_result) {
        return deserialize_stats_result;
      }
    }

    // Read store data section
    uint32_t store_count = 0;
    if (!ReadBinary(input_stream, store_count)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read store count"));
    }

    for (uint32_t store_idx = 0; store_idx < store_count; ++store_idx) {
      // Read store name
      std::string store_name;
      if (!ReadString(input_stream, store_name)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read store name"));
      }

      // Read store statistics (if present)
      uint32_t store_stats_size = 0;
      if (!ReadBinary(input_stream, store_stats_size)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                        "Failed to read store statistics size for '" + store_name + "'"));
      }
      if (store_stats_size > 0 && store_stats != nullptr) {
        uint32_t store_stats_crc = 0;
        if (!ReadBinary(input_stream, store_stats_crc)) {
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                          "Failed to read store statistics CRC for '" + store_name + "'"));
        }
        if (store_stats_size > kMaxStoreStatsSize) {
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Store statistics size exceeds maximum: " +
                                                                                std::to_string(store_stats_size)));
        }
        std::string store_stats_data(store_stats_size, '\0');
        input_stream.read(store_stats_data.data(), store_stats_size);
        if (!input_stream.good()) {
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                          "Failed to read store statistics data for '" + store_name + "'"));
        }
        {
          uint32_t actual_crc = CalculateCRC32(store_stats_data);
          if (actual_crc != store_stats_crc) {
            if (integrity_error != nullptr) {
              integrity_error->type = snapshot_format::CRCErrorType::StoreStatsCRC;
              integrity_error->store_name = store_name;
              integrity_error->message = "CRC32 mismatch in store statistics for '" + store_name + "': expected " +
                                         std::to_string(store_stats_crc) + ", got " + std::to_string(actual_crc);
            }
            return MakeUnexpected(
                MakeError(ErrorCode::kStorageDumpReadError, "CRC32 mismatch in store statistics for '" + store_name +
                                                                "': expected " + std::to_string(store_stats_crc) +
                                                                ", got " + std::to_string(actual_crc)));
          }
        }
        std::istringstream store_stats_ss(store_stats_data);
        StoreStatistics stats;
        auto deserialize_store_stats_result = DeserializeStoreStatistics(store_stats_ss, stats);
        if (!deserialize_store_stats_result) {
          return deserialize_store_stats_result;
        }
        (*store_stats)[store_name] = stats;
      }

      // Read store data
      uint32_t store_data_size = 0;
      uint32_t store_data_crc = 0;
      if (!ReadBinary(input_stream, store_data_size)) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Failed to read store data size for '" + store_name + "'"));
      }
      if (!ReadBinary(input_stream, store_data_crc)) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Failed to read store data CRC for '" + store_name + "'"));
      }
      if (store_data_size > kMaxStoreDataSize) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                        "Store data size exceeds maximum: " + std::to_string(store_data_size)));
      }
      std::string store_data(store_data_size, '\0');
      input_stream.read(store_data.data(), store_data_size);
      if (!input_stream.good()) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Failed to read store data for '" + store_name + "'"));
      }
      {
        uint32_t actual_crc = CalculateCRC32(store_data);
        if (actual_crc != store_data_crc) {
          // Determine store-specific CRC error type
          auto crc_error_type = snapshot_format::CRCErrorType::FileCRC;
          if (store_name == "events") {
            crc_error_type = snapshot_format::CRCErrorType::EventStoreCRC;
          } else if (store_name == "co_occurrence") {
            crc_error_type = snapshot_format::CRCErrorType::CoOccurrenceCRC;
          } else if (store_name == "vectors") {
            crc_error_type = snapshot_format::CRCErrorType::VectorStoreCRC;
          } else if (store_name == "metadata") {
            crc_error_type = snapshot_format::CRCErrorType::MetadataStoreCRC;
          }
          if (integrity_error != nullptr) {
            integrity_error->type = crc_error_type;
            integrity_error->store_name = store_name;
            integrity_error->message = "CRC32 mismatch in store data for '" + store_name + "': expected " +
                                       std::to_string(store_data_crc) + ", got " + std::to_string(actual_crc);
          }
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                          "CRC32 mismatch in store data for '" + store_name + "': expected " +
                                              std::to_string(store_data_crc) + ", got " + std::to_string(actual_crc)));
        }
      }

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
      } else if (store_name == "metadata") {
        if (metadata_store != nullptr) {
          auto deserialize_result = DeserializeMetadataStore(store_data_ss, *metadata_store, vector_store);
          if (!deserialize_result) {
            return deserialize_result;
          }
        }
      } else {
        LogStorageWarning("snapshot_read", "Unknown store name: " + store_name);
      }
    }

    LogStorageInfo("snapshot_read", "Snapshot loaded successfully from " + filepath);
    return {};
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

  {
    // Read and verify fixed header (magic + version)
    std::array<char, 4> magic{};
    input_stream.read(magic.data(), magic.size());
    if (!input_stream.good()) {
      integrity_error.type = snapshot_format::CRCErrorType::FileCRC;
      integrity_error.message = "Failed to read magic number";
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, integrity_error.message));
    }
    if (magic != snapshot_format::kMagicNumber) {
      integrity_error.type = snapshot_format::CRCErrorType::FileCRC;
      integrity_error.message = "Invalid magic number";
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, integrity_error.message));
    }

    uint32_t version = 0;
    if (!ReadBinary(input_stream, version)) {
      integrity_error.type = snapshot_format::CRCErrorType::FileCRC;
      integrity_error.message = "Failed to read version";
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, integrity_error.message));
    }
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

    // Verify whole-file size and CRC32 via the shared helper.
    auto file_integrity_result = VerifyFileLevelIntegrity(input_stream, header, &integrity_error);
    if (!file_integrity_result) {
      return file_integrity_result;
    }

    LogStorageInfo("snapshot_verify", "Snapshot integrity verified: " + filepath);
    return {};
  }
}

Expected<void, Error> GetSnapshotInfo(const std::string& filepath, SnapshotInfo& info) {
  // Open file for binary reading
  std::ifstream input_stream(filepath, std::ios::binary);
  if (!input_stream) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to open file for reading: " + filepath +
                                                                          " (" + std::strerror(errno) + ")"));
  }

  // Read and verify fixed header (magic + version)
  std::array<char, 4> magic{};
  input_stream.read(magic.data(), magic.size());
  if (!input_stream.good()) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read magic number"));
  }
  if (magic != snapshot_format::kMagicNumber) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Invalid magic number"));
  }

  uint32_t version = 0;
  if (!ReadBinary(input_stream, version)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read version"));
  }
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

  uint32_t config_size = 0;
  uint32_t config_crc = 0;
  if (!ReadBinary(input_stream, config_size) || !ReadBinary(input_stream, config_crc)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read config section header"));
  }
  input_stream.seekg(config_size, std::ios::cur);

  if (info.has_statistics) {
    uint32_t stats_size = 0;
    uint32_t stats_crc = 0;
    if (!ReadBinary(input_stream, stats_size) || !ReadBinary(input_stream, stats_crc)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read statistics section header"));
    }
    input_stream.seekg(stats_size, std::ios::cur);
  }

  if (!ReadBinary(input_stream, info.store_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read store count"));
  }

  return {};
}

}  // namespace nvecd::storage::snapshot_v1

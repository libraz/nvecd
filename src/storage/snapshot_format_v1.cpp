/**
 * @file snapshot_format_v1.cpp
 * @brief Snapshot file format Version 1 implementation
 *
 * Reference: ../mygram-db/src/storage/dump_format_v1.cpp
 * Reusability: 85% (adapted for nvecd stores)
 */

#include "storage/snapshot_format_v1.h"

#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_set>
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
constexpr uint32_t kMaxStringLength = 256 * 1024 * 1024;   // 256MB max for length-prefixed strings
constexpr uint64_t kMaxSnapshotFileSize = 3ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaxVectorDimension = 4096;  // Matches the public configuration schema.

enum class MetadataValueType : uint8_t {
  kString = 1,
  kInt64 = 2,
  kDouble = 3,
  kBool = 4,
};

/// @brief Absolute file offset of the file_crc32 field in the V1 header.
/// kFixedHeaderSize (8) + header_size(4) + flags(4) + snapshot_timestamp(8) + total_file_size(8) = 32
constexpr size_t kFileCRC32Offset = snapshot_format::kFixedHeaderSize + 24;

Expected<uint32_t, Error> CalculateFileCRC32Streaming(std::istream& input_stream, uint64_t file_size) {
  if (file_size > kMaxSnapshotFileSize) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Snapshot exceeds maximum total file size"));
  }
  input_stream.clear();
  input_stream.seekg(0, std::ios::beg);
  if (!input_stream.good()) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to seek snapshot for CRC32"));
  }

  std::array<char, 64 * 1024> buffer{};
  uint64_t offset = 0;
  uLong crc = crc32(0L, Z_NULL, 0);
  while (offset < file_size) {
    const size_t chunk_size = static_cast<size_t>(std::min<uint64_t>(buffer.size(), file_size - offset));
    input_stream.read(buffer.data(), static_cast<std::streamsize>(chunk_size));
    if (input_stream.gcount() != static_cast<std::streamsize>(chunk_size)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to stream snapshot CRC32"));
    }
    const uint64_t crc_begin = kFileCRC32Offset;
    const uint64_t crc_end = crc_begin + sizeof(uint32_t);
    const uint64_t chunk_end = offset + chunk_size;
    if (offset < crc_end && chunk_end > crc_begin) {
      const size_t zero_begin = static_cast<size_t>(std::max<uint64_t>(offset, crc_begin) - offset);
      const size_t zero_end = static_cast<size_t>(std::min<uint64_t>(chunk_end, crc_end) - offset);
      std::fill(buffer.begin() + static_cast<ptrdiff_t>(zero_begin), buffer.begin() + static_cast<ptrdiff_t>(zero_end),
                '\0');
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    crc = crc32(crc, reinterpret_cast<const Bytef*>(buffer.data()), static_cast<uInt>(chunk_size));
    offset = chunk_end;
  }
  return static_cast<uint32_t>(crc);
}

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

bool HasRemainingBytes(std::istream& input_stream, uint64_t required) {
  const auto current = input_stream.tellg();
  if (current == std::istream::pos_type(-1)) {
    return false;
  }
  input_stream.seekg(0, std::ios::end);
  const auto end = input_stream.tellg();
  input_stream.seekg(current, std::ios::beg);
  return end != std::istream::pos_type(-1) && end >= current && static_cast<uint64_t>(end - current) >= required;
}

/**
 * @brief Write string to stream (length-prefixed)
 */
bool WriteString(std::ostream& output_stream, const std::string& str) {
  if (str.size() > kMaxStringLength) {
    return false;
  }
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
  uint32_t len = 0;
  if (!ReadBinary(input_stream, len)) {
    return false;
  }
  if (len > kMaxStringLength) {
    LogStorageError("snapshot_read", "string_length_exceeded",
                    "String length " + std::to_string(len) + " exceeds limit");
    return false;
  }
  if (!HasRemainingBytes(input_stream, len)) {
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

Expected<uint32_t, Error> CheckedSizeToU32(size_t size, uint32_t limit, const std::string& label) {
  if (size > limit || size > std::numeric_limits<uint32_t>::max()) {
    return MakeUnexpected(
        MakeError(ErrorCode::kStorageDumpWriteError, label + " size exceeds maximum: " + std::to_string(size)));
  }
  return static_cast<uint32_t>(size);
}

Expected<void, Error> WriteSizedPayload(std::ostream& output_stream, const std::string& data, uint32_t limit,
                                        const std::string& label) {
  auto size = CheckedSizeToU32(data.size(), limit, label);
  if (!size) {
    return MakeUnexpected(size.error());
  }
  const uint32_t crc = CalculateCRC32(data);
  if (!WriteBinary(output_stream, *size) || !WriteBinary(output_stream, crc)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write " + label + " header"));
  }
  output_stream.write(data.data(), *size);
  if (!output_stream.good()) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write " + label + " data"));
  }
  return {};
}

bool IsFullyConsumed(std::istream& input_stream) {
  return input_stream.peek() == std::char_traits<char>::eof();
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
  const auto fail_file_integrity = [&](const std::string& message) -> Expected<void, Error> {
    if (integrity_error != nullptr) {
      integrity_error->type = snapshot_format::CRCErrorType::FileCRC;
      integrity_error->message = message;
    }
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, message));
  };
  // Verify file size against the value recorded in the header.
  input_stream.seekg(0, std::ios::end);
  const auto end_position = input_stream.tellg();
  if (end_position < 0) {
    return fail_file_integrity("Failed to determine snapshot file size");
  }
  const auto actual_file_size = static_cast<uint64_t>(end_position);
  if (actual_file_size > kMaxSnapshotFileSize || header.total_file_size > kMaxSnapshotFileSize) {
    return fail_file_integrity("Snapshot exceeds maximum total file size");
  }
  if (actual_file_size != header.total_file_size) {
    std::string message = "File size mismatch: expected " + std::to_string(header.total_file_size) + ", got " +
                          std::to_string(actual_file_size);
    return fail_file_integrity(message);
  }

  if ((header.flags & snapshot_format::flags_v1::kWithCRC) == 0) {
    return fail_file_integrity("Snapshot does not declare required CRC32 protection");
  }
  if (header.file_crc32 == 0) {
    return fail_file_integrity("Snapshot file CRC32 is missing");
  }
  auto computed_crc = CalculateFileCRC32Streaming(input_stream, actual_file_size);
  if (!computed_crc) {
    return MakeUnexpected(computed_crc.error());
  }
  if (*computed_crc != header.file_crc32) {
    std::string message =
        "File CRC32 mismatch: expected " + std::to_string(header.file_crc32) + ", got " + std::to_string(*computed_crc);
    return fail_file_integrity(message);
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
  auto context_count = CheckedSizeToU32(contexts.size(), std::numeric_limits<uint32_t>::max(), "event context count");
  if (!context_count) {
    return MakeUnexpected(context_count.error());
  }
  if (!WriteBinary(output_stream, *context_count)) {
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
    auto event_count = CheckedSizeToU32(events.size(), std::numeric_limits<uint32_t>::max(), "event count");
    if (!event_count) {
      return MakeUnexpected(event_count.error());
    }
    if (!WriteBinary(output_stream, *event_count)) {
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
  if (!HasRemainingBytes(input_stream, static_cast<uint64_t>(context_count) * (sizeof(uint32_t) * 2))) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Context count exceeds remaining store data"));
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
    constexpr uint64_t kMinSerializedEventSize = sizeof(uint32_t) + sizeof(int) + sizeof(uint8_t) + sizeof(uint64_t);
    if (!HasRemainingBytes(input_stream, static_cast<uint64_t>(event_count) * kMinSerializedEventSize)) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpReadError, "Event count exceeds remaining data for context: " + ctx));
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
  auto item_count = CheckedSizeToU32(items.size(), std::numeric_limits<uint32_t>::max(), "co-occurrence item count");
  if (!item_count) {
    return MakeUnexpected(item_count.error());
  }
  if (!WriteBinary(output_stream, *item_count)) {
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
    auto co_item_count =
        CheckedSizeToU32(co_items.size(), std::numeric_limits<uint32_t>::max(), "co-occurrence neighbor count");
    if (!co_item_count) {
      return MakeUnexpected(co_item_count.error());
    }
    if (!WriteBinary(output_stream, *co_item_count)) {
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
  if (!HasRemainingBytes(input_stream, static_cast<uint64_t>(item_count) * (sizeof(uint32_t) * 2))) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Item count exceeds remaining store data"));
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
    constexpr uint64_t kMinSerializedNeighborSize = sizeof(uint32_t) + sizeof(float);
    if (!HasRemainingBytes(input_stream, static_cast<uint64_t>(co_item_count) * kMinSerializedNeighborSize)) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpReadError, "Neighbor count exceeds remaining data for: " + item1));
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
  auto vector_count = CheckedSizeToU32(ids.size(), std::numeric_limits<uint32_t>::max(), "vector count");
  if (!vector_count) {
    return MakeUnexpected(vector_count.error());
  }
  if (!WriteBinary(output_stream, *vector_count)) {
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
  if (dimension > kMaxVectorDimension || dimension > std::numeric_limits<size_t>::max() / sizeof(float)) {
    return MakeUnexpected(
        MakeError(ErrorCode::kStorageDumpReadError, "Vector dimension exceeds maximum: " + std::to_string(dimension)));
  }

  // Read vector count
  uint32_t vector_count = 0;
  if (!ReadBinary(input_stream, vector_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read vector count"));
  }
  if (vector_count > 0 && dimension == 0) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Non-empty vector store has zero dimension"));
  }
  const uint64_t min_vector_size = sizeof(uint32_t) + dimension * sizeof(float);
  if (!HasRemainingBytes(input_stream, static_cast<uint64_t>(vector_count) * min_vector_size)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Vector count exceeds remaining store data"));
  }

  // Read each vector
  std::unordered_set<std::string> seen_vector_ids;
  for (uint32_t vec_idx = 0; vec_idx < vector_count; ++vec_idx) {
    // Read vector ID
    std::string vector_id;
    if (!ReadString(input_stream, vector_id)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read vector ID"));
    }
    if (!seen_vector_ids.insert(vector_id).second) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Duplicate vector ID: " + vector_id));
    }

    // Read vector data
    const uint64_t vector_bytes = dimension * sizeof(float);
    if (!HasRemainingBytes(input_stream, vector_bytes)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Vector exceeds remaining store data"));
    }
    std::vector<float> data(static_cast<size_t>(dimension));
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

  auto item_count = CheckedSizeToU32(entries.size(), std::numeric_limits<uint32_t>::max(), "metadata item count");
  if (!item_count) {
    return MakeUnexpected(item_count.error());
  }
  if (!WriteBinary(output_stream, *item_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write metadata item count"));
  }

  for (const auto& [id, metadata] : entries) {
    if (!WriteString(output_stream, id)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write metadata item ID: " + id));
    }

    auto field_count = CheckedSizeToU32(metadata.size(), std::numeric_limits<uint32_t>::max(), "metadata field count");
    if (!field_count) {
      return MakeUnexpected(field_count.error());
    }
    if (!WriteBinary(output_stream, *field_count)) {
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
  if (!HasRemainingBytes(input_stream, static_cast<uint64_t>(item_count) * (sizeof(uint32_t) * 2))) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Metadata item count exceeds remaining data"));
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
    constexpr uint64_t kMinSerializedMetadataFieldSize = sizeof(uint32_t) + sizeof(uint8_t) + sizeof(bool);
    if (!HasRemainingBytes(input_stream, static_cast<uint64_t>(field_count) * kMinSerializedMetadataFieldSize)) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpReadError, "Metadata field count exceeds remaining data for: " + id));
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
                                      const vectors::MetadataStore* metadata_store, bool suppress_logging,
                                      const SnapshotWriteLimits* limits) {
  const SnapshotWriteLimits default_limits;
  const auto& write_limits = limits != nullptr ? *limits : default_limits;
  const uint32_t max_config_size = std::min(kMaxConfigSize, write_limits.max_config_size);
  const uint32_t max_stats_size = std::min(kMaxStatsSize, write_limits.max_stats_size);
  const uint32_t max_store_data_size = std::min(kMaxStoreDataSize, write_limits.max_store_data_size);
  const uint32_t max_store_stats_size = std::min(kMaxStoreStatsSize, write_limits.max_store_stats_size);
  auto storage_target_result = PrivateStorageTarget::Open(filepath);
  if (!storage_target_result) {
    return MakeUnexpected(storage_target_result.error());
  }
  auto storage_target = std::move(storage_target_result.value());
  const std::string resolved_filepath = storage_target.DisplayPath();

  auto temp_file_result = storage_target.CreateTemporaryFile();
  if (!temp_file_result) {
    return MakeUnexpected(temp_file_result.error());
  }
  auto temp_file = std::move(temp_file_result.value());
  const std::string temp_stream_path = temp_file.StreamPath();

  // The /dev/fd path refers to the already validated open inode. It cannot be
  // redirected by replacing the temporary directory entry.
  std::ofstream output_stream(temp_stream_path, std::ios::binary | std::ios::trunc);
  if (!output_stream) {
    return MakeUnexpected(
        MakeError(ErrorCode::kStorageDumpWriteError,
                  "Failed to open secure temporary snapshot stream (" + std::string(std::strerror(errno)) + ")"));
  }

  {
    // Write fixed header (magic + version)
    output_stream.write(snapshot_format::kMagicNumber.data(), snapshot_format::kMagicNumber.size());
    if (!output_stream.good()) {
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
    auto header_size = CheckedSizeToU32(header_ss.str().size(), kMaxConfigSize, "snapshot header");
    if (!header_size) {
      return MakeUnexpected(header_size.error());
    }
    header.header_size = *header_size;

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
    auto config_write = WriteSizedPayload(output_stream, config_data, max_config_size, "config section");
    if (!config_write) {
      return config_write;
    }

    // Write statistics section (if provided)
    if (stats != nullptr) {
      std::ostringstream stats_ss;
      auto stats_result = SerializeStatistics(stats_ss, *stats);
      if (!stats_result) {
        return stats_result;
      }
      std::string stats_data = stats_ss.str();
      auto stats_write = WriteSizedPayload(output_stream, stats_data, max_stats_size, "statistics section");
      if (!stats_write) {
        return stats_write;
      }
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
        auto write_result =
            WriteSizedPayload(output_stream, store_stats_data, max_store_stats_size, "events store statistics");
        if (!write_result) {
          return write_result;
        }
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
      auto write_result = WriteSizedPayload(output_stream, store_data, max_store_data_size, "events store");
      if (!write_result) {
        return write_result;
      }
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
        auto write_result =
            WriteSizedPayload(output_stream, store_stats_data, max_store_stats_size, "co-occurrence store statistics");
        if (!write_result) {
          return write_result;
        }
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
      auto write_result = WriteSizedPayload(output_stream, store_data, max_store_data_size, "co-occurrence store");
      if (!write_result) {
        return write_result;
      }
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
        auto write_result =
            WriteSizedPayload(output_stream, store_stats_data, max_store_stats_size, "vectors store statistics");
        if (!write_result) {
          return write_result;
        }
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
      auto write_result = WriteSizedPayload(output_stream, store_data, max_store_data_size, "vectors store");
      if (!write_result) {
        return write_result;
      }
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
      auto write_result = WriteSizedPayload(output_stream, store_data, max_store_data_size, "metadata store");
      if (!write_result) {
        return write_result;
      }
    }

    // Check stream state after all store writes
    if (!output_stream.good()) {
      output_stream.close();
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Stream error during snapshot write"));
    }

    // Calculate total file size
    header.total_file_size = static_cast<uint64_t>(output_stream.tellp());
    if (header.total_file_size > kMaxSnapshotFileSize) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Snapshot exceeds maximum total file size"));
    }

    // Write header with correct total_file_size (file_crc32 still 0 as placeholder)
    header.file_crc32 = 0;
    output_stream.seekp(snapshot_format::kFixedHeaderSize, std::ios::beg);
    auto rewrite_header_result = WriteHeaderV1(output_stream, header);
    if (!rewrite_header_result) {
      return rewrite_header_result;
    }

    // Close file to flush all data and surface buffered write failures.
    output_stream.close();
    if (output_stream.fail()) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to close snapshot temp file"));
    }

    // Compute file-level CRC32: read entire file, zero the file_crc32 field, compute CRC
    {
      std::ifstream crc_input(temp_stream_path, std::ios::binary);
      if (!crc_input) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpWriteError, "Failed to reopen secure temp inode for CRC32 computation"));
      }
      auto file_crc = CalculateFileCRC32Streaming(crc_input, header.total_file_size);
      if (!file_crc) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, file_crc.error().message()));
      }
      crc_input.close();
      if (crc_input.fail()) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to close snapshot CRC input"));
      }

      header.file_crc32 = *file_crc;

      // Write the computed CRC32 at the file_crc32 position
      std::ofstream crc_output(temp_stream_path, std::ios::binary | std::ios::in | std::ios::out);
      if (!crc_output) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpWriteError, "Failed to reopen secure temp inode for CRC32 write"));
      }
      crc_output.seekp(static_cast<std::streamoff>(kFileCRC32Offset), std::ios::beg);
      if (!WriteBinary(crc_output, header.file_crc32)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write snapshot file CRC32"));
      }
      crc_output.close();
      if (crc_output.fail()) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to close snapshot CRC output"));
      }
    }

#ifndef _WIN32
    // Persist the same opened inode that was written and CRC-checked.
    if (::fsync(temp_file.Get()) != 0) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError,
                                      "Failed to fsync snapshot temp file: " + std::string(std::strerror(errno))));
    }
#endif

    auto publish_result = storage_target.Publish(temp_file);
    if (!publish_result) {
      return publish_result;
    }

#ifndef _WIN32
    auto directory_sync_result = storage_target.FsyncDirectory();
    if (!directory_sync_result) {
      return directory_sync_result;
    }
#endif

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
  auto storage_target_result = PrivateStorageTarget::Open(filepath);
  if (!storage_target_result) {
    return MakeUnexpected(storage_target_result.error());
  }
  auto storage_target = std::move(storage_target_result.value());
  auto snapshot_fd_result = storage_target.OpenRegularFileReadOnly();
  if (!snapshot_fd_result) {
    return MakeUnexpected(snapshot_fd_result.error());
  }
  auto snapshot_fd = std::move(snapshot_fd_result.value());
  std::ifstream input_stream(snapshot_fd.StreamPath(), std::ios::binary);
  if (!input_stream) {
    return MakeUnexpected(MakeError(
        ErrorCode::kStorageDumpReadError,
        "Failed to open validated snapshot inode for reading: " + filepath + " (" + std::strerror(errno) + ")"));
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
    if (!HasRemainingBytes(input_stream, config_size)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Config section exceeds remaining file"));
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
    if (!IsFullyConsumed(config_ss)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Trailing bytes in config section"));
    }

    // Read statistics section (if present)
    if ((header.flags & snapshot_format::flags_v1::kWithStatistics) != 0) {
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
      if (!HasRemainingBytes(input_stream, stats_size)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Statistics section exceeds remaining file"));
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
      if (stats != nullptr) {
        std::istringstream stats_ss(stats_data);
        auto deserialize_stats_result = DeserializeStatistics(stats_ss, *stats);
        if (!deserialize_stats_result) {
          return deserialize_stats_result;
        }
        if (!IsFullyConsumed(stats_ss)) {
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Trailing bytes in statistics section"));
        }
      }
    }

    // Read store data section
    uint32_t store_count = 0;
    if (!ReadBinary(input_stream, store_count)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read store count"));
    }
    if (store_count < 3 || store_count > 4) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpReadError, "Snapshot store count must be between 3 and 4"));
    }

    std::unordered_set<std::string> seen_stores;
    vectors::MetadataStore discarded_metadata_store;
    for (uint32_t store_idx = 0; store_idx < store_count; ++store_idx) {
      // Read store name
      std::string store_name;
      if (!ReadString(input_stream, store_name)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read store name"));
      }
      if (store_name != "events" && store_name != "co_occurrence" && store_name != "vectors" &&
          store_name != "metadata") {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Unknown store name: '" + store_name + "'"));
      }
      if (!seen_stores.insert(store_name).second) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Duplicate store section: '" + store_name + "'"));
      }

      // Read store statistics (if present)
      uint32_t store_stats_size = 0;
      if (!ReadBinary(input_stream, store_stats_size)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                        "Failed to read store statistics size for '" + store_name + "'"));
      }
      if (store_stats_size > 0) {
        uint32_t store_stats_crc = 0;
        if (!ReadBinary(input_stream, store_stats_crc)) {
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                          "Failed to read store statistics CRC for '" + store_name + "'"));
        }
        if (store_stats_size > kMaxStoreStatsSize) {
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Store statistics size exceeds maximum: " +
                                                                                std::to_string(store_stats_size)));
        }
        if (!HasRemainingBytes(input_stream, store_stats_size)) {
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Store statistics exceed remaining file"));
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
        if (store_stats != nullptr) {
          std::istringstream store_stats_ss(store_stats_data);
          StoreStatistics parsed_stats;
          auto deserialize_store_stats_result = DeserializeStoreStatistics(store_stats_ss, parsed_stats);
          if (!deserialize_store_stats_result) {
            return deserialize_store_stats_result;
          }
          if (!IsFullyConsumed(store_stats_ss)) {
            return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                            "Trailing bytes in store statistics for '" + store_name + "'"));
          }
          (*store_stats)[store_name] = parsed_stats;
        }
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
      if (!HasRemainingBytes(input_stream, store_data_size)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Store data exceeds remaining file"));
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
        auto& metadata_target = metadata_store != nullptr ? *metadata_store : discarded_metadata_store;
        auto deserialize_result = DeserializeMetadataStore(store_data_ss, metadata_target, vector_store);
        if (!deserialize_result) {
          return deserialize_result;
        }
      }
      if (!IsFullyConsumed(store_data_ss)) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Trailing bytes in store data for '" + store_name + "'"));
      }
    }

    for (const std::string required_store : {"events", "co_occurrence", "vectors"}) {
      if (seen_stores.count(required_store) == 0) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Missing required store: '" + required_store + "'"));
      }
    }
    if (seen_stores.count("metadata") > 0) {
      const auto* validated_metadata = metadata_store != nullptr ? metadata_store : &discarded_metadata_store;
      auto metadata_lock = validated_metadata->AcquireReadLock();
      for (const auto& [item_id, _] : validated_metadata->GetAll()) {
        if (!vector_store.HasVector(item_id)) {
          return MakeUnexpected(
              MakeError(ErrorCode::kStorageDumpReadError, "Metadata references missing vector: '" + item_id + "'"));
        }
      }
    }
    if (!IsFullyConsumed(input_stream)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Trailing bytes after final store section"));
    }

    LogStorageInfo("snapshot_read", "Snapshot loaded successfully from " + filepath);
    return {};
  }
}

Expected<void, Error> VerifySnapshotIntegrity(const std::string& filepath,
                                              snapshot_format::IntegrityError& integrity_error) {
  auto storage_target_result = PrivateStorageTarget::Open(filepath);
  if (!storage_target_result) {
    integrity_error.type = snapshot_format::CRCErrorType::FileCRC;
    integrity_error.message = storage_target_result.error().message();
    return MakeUnexpected(storage_target_result.error());
  }
  auto storage_target = std::move(storage_target_result.value());
  auto snapshot_fd_result = storage_target.OpenRegularFileReadOnly();
  if (!snapshot_fd_result) {
    integrity_error.type = snapshot_format::CRCErrorType::FileCRC;
    integrity_error.message = snapshot_fd_result.error().message();
    return MakeUnexpected(snapshot_fd_result.error());
  }
  auto snapshot_fd = std::move(snapshot_fd_result.value());
  std::ifstream input_stream(snapshot_fd.StreamPath(), std::ios::binary);
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
  auto storage_target_result = PrivateStorageTarget::Open(filepath);
  if (!storage_target_result) {
    return MakeUnexpected(storage_target_result.error());
  }
  auto storage_target = std::move(storage_target_result.value());
  auto snapshot_fd_result = storage_target.OpenRegularFileReadOnly();
  if (!snapshot_fd_result) {
    return MakeUnexpected(snapshot_fd_result.error());
  }
  auto snapshot_fd = std::move(snapshot_fd_result.value());
  std::ifstream input_stream(snapshot_fd.StreamPath(), std::ios::binary);
  if (!input_stream) {
    return MakeUnexpected(MakeError(
        ErrorCode::kStorageDumpReadError,
        "Failed to open validated snapshot inode for reading: " + filepath + " (" + std::strerror(errno) + ")"));
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

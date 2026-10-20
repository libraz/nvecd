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
#include <cstdint>
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

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nvecd::storage::snapshot_v1 {

using namespace utils;

namespace {

constexpr uint32_t kMaxStringLength = 256 * 1024 * 1024;  // 256MB max for length-prefixed strings
/// Reader-side dimension bound. Shares the store's single limit so a snapshot
/// can never carry a dimension the live store would refuse.
constexpr uint64_t kMaxVectorDimension = vectors::VectorStore::kMaxDimension;

/// Store section names as they appear on disk.
constexpr const char* kEventsStoreName = "events";
constexpr const char* kCoOccurrenceStoreName = "co_occurrence";
constexpr const char* kVectorsStoreName = "vectors";
constexpr const char* kMetadataStoreName = "metadata";

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
 * @brief Running whole-file CRC32 built from the bytes as they are written or read
 *
 * The file CRC32 covers the file with its own field zeroed, so both directions
 * can fold bytes in file order and compare once at the end instead of making a
 * separate pass over the finished file.
 */
class FileCrc {
 public:
  void Fold(const void* data, size_t length) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* cursor = reinterpret_cast<const Bytef*>(data);
    size_t remaining = length;
    while (remaining > 0) {
      // zlib takes a uInt length, which is narrower than a section can be.
      const auto chunk = static_cast<uInt>(std::min<size_t>(remaining, kFoldChunkBytes));
      crc_ = crc32(crc_, cursor, chunk);
      cursor += chunk;
      remaining -= chunk;
    }
    length_ += length;
  }

  /// Append a run whose CRC32 and length are already known.
  void FoldRun(uint32_t run_crc, uint64_t run_length) {
    crc_ = crc32_combine(crc_, run_crc, static_cast<z_off_t>(run_length));
    length_ += run_length;
  }

  [[nodiscard]] uint32_t Value() const { return static_cast<uint32_t>(crc_); }
  [[nodiscard]] uint64_t Length() const { return length_; }

 private:
  static constexpr size_t kFoldChunkBytes = 1U << 20;

  uLong crc_ = crc32(0L, Z_NULL, 0);
  uint64_t length_ = 0;
};

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

bool WriteRawBytes(std::ostream& output_stream, const void* data, size_t size) {
  if (static_cast<std::uintmax_t>(size) > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    return false;
  }
  if (size > 0) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    output_stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  }
  return output_stream.good();
}

bool ReadRawBytes(std::istream& input_stream, void* data, size_t size) {
  if (static_cast<std::uintmax_t>(size) > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    return false;
  }
  if (size > 0) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    input_stream.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
  }
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

/**
 * @brief Read-only stream view over an existing buffer
 *
 * std::istringstream copies the string it is handed, so parsing a section that
 * had already been read into memory cost a second copy of it. This exposes the
 * bytes that are already there instead. Seeking is supported because the
 * section readers locate their own end through seekg/tellg.
 */
class BufferViewStreambuf : public std::streambuf {
 public:
  BufferViewStreambuf(char* data, size_t size) { setg(data, data, data + size); }

 protected:
  pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode which) override {
    if ((which & std::ios_base::in) == 0) {
      return pos_type(off_type(-1));
    }
    off_type target = offset;
    if (direction == std::ios_base::cur) {
      target += gptr() - eback();
    } else if (direction == std::ios_base::end) {
      target += egptr() - eback();
    }
    if (target < 0 || target > egptr() - eback()) {
      return pos_type(off_type(-1));
    }
    setg(eback(), eback() + target, egptr());
    return pos_type(target);
  }

  pos_type seekpos(pos_type position, std::ios_base::openmode which) override {
    return seekoff(off_type(position), std::ios_base::beg, which);
  }
};

/**
 * @brief Output filter that forwards bytes onward and accumulates their CRC32
 *
 * Store sections are written straight into the snapshot file rather than being
 * built in memory first, so peak memory stays a constant buffer plus the store
 * itself instead of twice the serialized section. The size and CRC32 that
 * precede a section are only known once it ends, so they are written as
 * placeholders and patched afterwards; this filter supplies the CRC without a
 * second pass over the bytes.
 */
class CrcTeeStreambuf : public std::streambuf {
 public:
  explicit CrcTeeStreambuf(std::ostream& sink) : sink_(sink) {}

  [[nodiscard]] FileCrc& Tracker() { return crc_; }
  [[nodiscard]] uint32_t Crc() const { return crc_.Value(); }

 protected:
  std::streamsize xsputn(const char* data, std::streamsize count) override {
    sink_.write(data, count);
    if (!sink_.good()) {
      return 0;
    }
    crc_.Fold(data, static_cast<size_t>(count));
    return count;
  }

  int_type overflow(int_type ch) override {
    if (ch == traits_type::eof()) {
      return traits_type::not_eof(ch);
    }
    const char byte = static_cast<char>(ch);
    return xsputn(&byte, 1) == 1 ? ch : traits_type::eof();
  }

 private:
  std::ostream& sink_;
  FileCrc crc_;
};

/**
 * @brief Write a section as [size:u32][crc32:u32][payload], streaming the payload
 *
 * The bytes are identical to those produced by materializing the section first,
 * so snapshots written before and after this change are interchangeable.
 *
 * @param output_stream Seekable destination
 * @param file_crc Whole-file checksum that receives the finished section in file order
 * @param limit Largest payload this section may occupy
 * @param label Section name used in error messages
 * @param serialize Callable invoked with the payload stream
 * @return Success, or the serializer's error / a kStorageDumpWriteError
 */
template <typename SerializeFn>
Expected<void, Error> WriteStreamedSection(std::ostream& output_stream, FileCrc& file_crc, uint32_t limit,
                                           const std::string& label, SerializeFn&& serialize) {
  const std::streampos header_pos = output_stream.tellp();
  if (header_pos == std::streampos(-1)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Cannot locate " + label + " header"));
  }
  const uint32_t placeholder = 0;
  if (!WriteBinary(output_stream, placeholder) || !WriteBinary(output_stream, placeholder)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write " + label + " header"));
  }
  const std::streampos data_pos = output_stream.tellp();

  uint32_t crc = 0;
  {
    CrcTeeStreambuf tee(output_stream);
    std::ostream tee_stream(&tee);
    auto serialized = serialize(tee_stream);
    if (!serialized) {
      return serialized;
    }
    tee_stream.flush();
    crc = tee.Crc();
  }
  if (!output_stream.good()) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write " + label + " data"));
  }

  const std::streampos end_pos = output_stream.tellp();
  auto size = CheckedSizeToU32(static_cast<size_t>(end_pos - data_pos), limit, label);
  if (!size) {
    return MakeUnexpected(size.error());
  }

  output_stream.seekp(header_pos, std::ios::beg);
  if (!WriteBinary(output_stream, *size) || !WriteBinary(output_stream, crc)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to patch " + label + " header"));
  }
  file_crc.Fold(&*size, sizeof(uint32_t));
  file_crc.Fold(&crc, sizeof(uint32_t));
  file_crc.FoldRun(crc, *size);
  output_stream.seekp(end_pos, std::ios::beg);
  if (!output_stream.good()) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to resume after " + label + " header"));
  }
  return {};
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

Expected<void, Error> FailFileIntegrity(snapshot_format::IntegrityError* integrity_error, const std::string& message) {
  if (integrity_error != nullptr) {
    integrity_error->type = snapshot_format::CRCErrorType::FileCRC;
    integrity_error->message = message;
  }
  return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, message));
}

/**
 * @brief Check the header's size and CRC declarations against the file, without reading its bytes
 *
 * Leaves the stream positioned at the end of the file.
 */
Expected<void, Error> CheckFileBounds(std::istream& input_stream, const HeaderV1& header,
                                      snapshot_format::IntegrityError* integrity_error) {
  input_stream.seekg(0, std::ios::end);
  const auto end_position = input_stream.tellg();
  if (end_position < 0) {
    return FailFileIntegrity(integrity_error, "Failed to determine snapshot file size");
  }
  const auto actual_file_size = static_cast<uint64_t>(end_position);
  if (actual_file_size > kMaxSnapshotFileSize || header.total_file_size > kMaxSnapshotFileSize) {
    return FailFileIntegrity(integrity_error, "Snapshot exceeds maximum total file size");
  }
  if (actual_file_size != header.total_file_size) {
    return FailFileIntegrity(integrity_error, "File size mismatch: expected " + std::to_string(header.total_file_size) +
                                                  ", got " + std::to_string(actual_file_size));
  }
  if ((header.flags & snapshot_format::flags_v1::kWithCRC) == 0) {
    return FailFileIntegrity(integrity_error, "Snapshot does not declare required CRC32 protection");
  }
  return {};
}

Expected<void, Error> CheckFileCrc(uint32_t actual_crc, const HeaderV1& header,
                                   snapshot_format::IntegrityError* integrity_error) {
  if (actual_crc != header.file_crc32) {
    return FailFileIntegrity(integrity_error, "File CRC32 mismatch: expected " + std::to_string(header.file_crc32) +
                                                  ", got " + std::to_string(actual_crc));
  }
  return {};
}

/**
 * @brief Verify the whole-file size and CRC32 recorded in the V1 header with a pass over the file.
 *
 * Used where the file is checked on its own (DUMP VERIFY) and to classify a
 * failed load, whose single-pass read cannot say whether the file was damaged.
 */
Expected<void, Error> VerifyFileLevelIntegrity(std::istream& input_stream, const HeaderV1& header,
                                               snapshot_format::IntegrityError* integrity_error) {
  auto bounds = CheckFileBounds(input_stream, header, integrity_error);
  if (!bounds) {
    return bounds;
  }
  auto computed_crc = CalculateFileCRC32Streaming(input_stream, header.total_file_size);
  if (!computed_crc) {
    return MakeUnexpected(computed_crc.error());
  }
  return CheckFileCrc(*computed_crc, header, integrity_error);
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
      // preserved. RestoreEvent appends without deduplicating, so the buffer
      // matches the snapshot, and reseeds the ADD dedup window and SET state
      // cache from each restored event.
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

      // Restore the score verbatim, but through the same validation the live
      // path uses. Verbatim applies to the value, not to the invariants: a
      // self-edge corrupts the map during pruning and a non-finite score breaks
      // the ordering of every later sort, so a snapshot carrying one is
      // rejected rather than partially applied. Loading it and dropping the
      // edge would be worse — the file would appear to load while the restored
      // index differed from the one that was written.
      auto restored = co_index.SetScore(item1, item2, score);
      if (!restored) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                        "Failed to restore co-occurrence edge: " + restored.error().message()));
      }
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
  // The snapshot is taken first so the dimension and the rows cannot diverge; it
  // is held for the whole section so rows are read in place, not copied per id.
  auto snap = vector_store.GetCompactSnapshot();
  auto dimension = static_cast<uint64_t>(vector_store.GetDimension());
  if (!WriteBinary(output_stream, dimension)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write dimension"));
  }
  const size_t live_count = snap.Empty() ? 0 : snap.id_to_idx->size();

  // Write vector count
  auto vector_count = CheckedSizeToU32(live_count, std::numeric_limits<uint32_t>::max(), "vector count");
  if (!vector_count) {
    return MakeUnexpected(vector_count.error());
  }
  if (!WriteBinary(output_stream, *vector_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write vector count"));
  }

  // Write each live row, skipping tombstones
  const size_t vector_bytes = snap.dim * sizeof(float);
  for (size_t row = 0; row < snap.count; ++row) {
    if (snap.IsDeleted(row)) {
      continue;
    }
    const std::string& vector_id = (*snap.idx_to_id)[row];
    if (!WriteString(output_stream, vector_id)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write vector ID: " + vector_id));
    }
    if (!WriteRawBytes(output_stream, snap.matrix + row * snap.dim, vector_bytes)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write vector data"));
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
  std::vector<float> data(static_cast<size_t>(dimension));
  for (uint32_t vec_idx = 0; vec_idx < vector_count; ++vec_idx) {
    // Read vector ID
    std::string vector_id;
    if (!ReadString(input_stream, vector_id)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read vector ID"));
    }
    if (vector_store.HasVector(vector_id)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Duplicate vector ID: " + vector_id));
    }

    // Read vector data
    const uint64_t vector_bytes = dimension * sizeof(float);
    if (!HasRemainingBytes(input_stream, vector_bytes)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Vector exceeds remaining store data"));
    }
    if (!ReadRawBytes(input_stream, data.data(), static_cast<size_t>(vector_bytes))) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read vector data"));
    }

    // Add vector to store. Vectors are persisted verbatim (already in their
    // stored form), so no re-normalization is applied on load.
    auto result = vector_store.SetVector(vector_id, data, false);
    if (!result) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpReadError, "Failed to add vector: " + result.error().message()));
    }
  }

  // A corpus that was emptied before it was persisted still carries the
  // dimension it was built for. Without restoring it here the reloaded store
  // has no dimension at all and accepts the next vector at any size, so a
  // mismatched embedding model is admitted instead of rejected.
  if (vector_count == 0 && dimension > 0) {
    auto restored = vector_store.RestoreDimension(static_cast<size_t>(dimension));
    if (!restored) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                      "Failed to restore vector dimension: " + restored.error().message()));
    }
  }

  return {};
}

// ============================================================================
// MetadataStore Serialization
// ============================================================================

Expected<void, Error> SerializeMetadataStore(std::ostream& output_stream,
                                             const vectors::MetadataStore& metadata_store) {
  // Snapshot the id -> metadata map under a read lock so the iteration is
  // consistent with concurrent writers.
  auto read_lock = metadata_store.AcquireReadLock();
  const auto& all = metadata_store.GetAll();
  size_t nonempty_count = 0;
  for (const auto& entry : all) {
    if (!entry.second.empty()) {
      ++nonempty_count;
    }
  }

  auto item_count = CheckedSizeToU32(nonempty_count, std::numeric_limits<uint32_t>::max(), "metadata item count");
  if (!item_count) {
    return MakeUnexpected(item_count.error());
  }
  if (!WriteBinary(output_stream, *item_count)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write metadata item count"));
  }

  for (const auto& [id, metadata] : all) {
    if (metadata.empty()) {
      continue;
    }
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

Expected<void, Error> DeserializeMetadataStore(std::istream& input_stream, vectors::MetadataStore& metadata_store) {
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

namespace {

/// Snapshot file stream plus the running checksum of everything written after the header.
struct SnapshotSink {
  explicit SnapshotSink(std::ostream& file_stream) : file(file_stream), tee(file_stream), tracked(&tee) {}

  std::ostream& file;    ///< The seekable file; streamed sections patch their headers here
  CrcTeeStreambuf tee;   ///< Forwards to `file` and folds each byte into the checksum
  std::ostream tracked;  ///< Writes through `tee`
  FileCrc& Crc() { return tee.Tracker(); }
};

/// Write one store section: name, optional statistics, then the streamed payload.
template <typename SerializeFn>
Expected<void, Error> WriteStoreSection(SnapshotSink& sink, const std::string& store_name, const std::string& label,
                                        const std::unordered_map<std::string, StoreStatistics>* store_stats,
                                        uint32_t max_stats_size, uint32_t max_data_size, SerializeFn&& serialize) {
  WriteString(sink.tracked, store_name);
  if (store_stats != nullptr && store_stats->count(store_name) > 0) {
    std::ostringstream store_stats_ss;
    auto store_stats_result = SerializeStoreStatistics(store_stats_ss, store_stats->at(store_name));
    if (!store_stats_result) {
      return store_stats_result;
    }
    auto stats_write =
        WriteSizedPayload(sink.tracked, store_stats_ss.str(), max_stats_size, label + " store statistics");
    if (!stats_write) {
      return stats_write;
    }
  } else {
    uint32_t zero = 0;
    WriteBinary(sink.tracked, zero);  // No store stats
  }
  return WriteStreamedSection(sink.file, sink.Crc(), max_data_size, label + " store",
                              std::forward<SerializeFn>(serialize));
}

/// A snapshot opened for reading with its fixed header and V1 header parsed.
struct OpenedSnapshot {
  SecureFileDescriptor fd;  ///< Keeps the validated inode open behind `stream`
  std::ifstream stream;     ///< Positioned at the first section
  uint32_t version = 0;
  HeaderV1 header;
};

Expected<OpenedSnapshot, Error> OpenSnapshotWithHeader(const std::string& filepath) {
  auto storage_target_result = PrivateStorageTarget::Open(filepath);
  if (!storage_target_result) {
    return MakeUnexpected(storage_target_result.error());
  }
  auto storage_target = std::move(storage_target_result.value());
  auto snapshot_fd_result = storage_target.OpenRegularFileReadOnly();
  if (!snapshot_fd_result) {
    return MakeUnexpected(snapshot_fd_result.error());
  }
  OpenedSnapshot opened;
  opened.fd = std::move(snapshot_fd_result.value());
  opened.stream.open(opened.fd.StreamPath(), std::ios::binary);
  if (!opened.stream) {
    return MakeUnexpected(MakeError(
        ErrorCode::kStorageDumpReadError,
        "Failed to open validated snapshot inode for reading: " + filepath + " (" + std::strerror(errno) + ")"));
  }

  // Read and verify fixed header (magic + version)
  std::array<char, 4> magic{};
  opened.stream.read(magic.data(), magic.size());
  if (!opened.stream.good()) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read magic number"));
  }
  if (magic != snapshot_format::kMagicNumber) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Invalid magic number"));
  }
  if (!ReadBinary(opened.stream, opened.version)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read version"));
  }
  if (opened.version < snapshot_format::kMinSupportedVersion ||
      opened.version > snapshot_format::kMaxSupportedVersion) {
    return MakeUnexpected(
        MakeError(ErrorCode::kStorageDumpReadError, "Unsupported version: " + std::to_string(opened.version)));
  }

  auto read_header_result = ReadHeaderV1(opened.stream, opened.header);
  if (!read_header_result) {
    return MakeUnexpected(read_header_result.error());
  }
  return opened;
}

/// CRC error class reported for a mismatch in the named store's data section.
snapshot_format::CRCErrorType StoreDataCrcErrorType(const std::string& store_name) {
  if (store_name == kEventsStoreName) {
    return snapshot_format::CRCErrorType::EventStoreCRC;
  }
  if (store_name == kCoOccurrenceStoreName) {
    return snapshot_format::CRCErrorType::CoOccurrenceCRC;
  }
  if (store_name == kVectorsStoreName) {
    return snapshot_format::CRCErrorType::VectorStoreCRC;
  }
  if (store_name == kMetadataStoreName) {
    return snapshot_format::CRCErrorType::MetadataStoreCRC;
  }
  return snapshot_format::CRCErrorType::FileCRC;
}

/// Where a sized section sits in the file, for bounds checks and error reporting.
struct SectionSpec {
  std::string label;  ///< Names the section in error messages
  uint32_t max_size = 0;
  snapshot_format::CRCErrorType crc_error_type = snapshot_format::CRCErrorType::FileCRC;
  std::string store_name;  ///< Empty for sections that belong to no store
};

/// Read a section whose size and CRC32 were already parsed: bound it, read it, verify its CRC32.
Expected<std::string, Error> ReadCheckedSection(std::istream& input_stream, FileCrc& file_crc, uint32_t size,
                                                uint32_t expected_crc, const SectionSpec& spec,
                                                snapshot_format::IntegrityError* integrity_error) {
  if (size > spec.max_size) {
    return MakeUnexpected(
        MakeError(ErrorCode::kStorageDumpReadError, spec.label + " size exceeds maximum: " + std::to_string(size)));
  }
  if (!HasRemainingBytes(input_stream, size)) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, spec.label + " exceeds remaining file"));
  }
  std::string data(size, '\0');
  input_stream.read(data.data(), size);
  if (!input_stream.good()) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read " + spec.label));
  }
  const uint32_t actual_crc = CalculateCRC32(data);
  if (actual_crc != expected_crc) {
    const std::string message = "CRC32 mismatch in " + spec.label + ": expected " + std::to_string(expected_crc) +
                                ", got " + std::to_string(actual_crc);
    if (integrity_error != nullptr) {
      integrity_error->type = spec.crc_error_type;
      if (!spec.store_name.empty()) {
        integrity_error->store_name = spec.store_name;
      }
      integrity_error->message = message;
    }
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, message));
  }
  file_crc.FoldRun(actual_crc, size);
  return data;
}

/// Read a fixed-size field and fold its bytes into the file checksum.
template <typename T>
bool ReadFolded(std::istream& input_stream, FileCrc& file_crc, T& value) {
  if (!ReadBinary(input_stream, value)) {
    return false;
  }
  file_crc.Fold(&value, sizeof(T));
  return true;
}

/// Read a length-prefixed string and fold its bytes into the file checksum.
bool ReadStringFolded(std::istream& input_stream, FileCrc& file_crc, std::string& str) {
  if (!ReadString(input_stream, str)) {
    return false;
  }
  const auto length = static_cast<uint32_t>(str.size());
  file_crc.Fold(&length, sizeof(length));
  file_crc.Fold(str.data(), str.size());
  return true;
}

/// Fold the bytes before the first section into the file checksum, with the CRC field zeroed.
Expected<void, Error> FoldHeaderPrefix(std::istream& input_stream, std::streampos sections_begin, FileCrc& file_crc) {
  std::string prefix(static_cast<size_t>(sections_begin), '\0');
  input_stream.clear();
  input_stream.seekg(0, std::ios::beg);
  input_stream.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
  if (!input_stream.good()) {
    return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to re-read snapshot header"));
  }
  std::fill_n(prefix.begin() + static_cast<ptrdiff_t>(kFileCRC32Offset), sizeof(uint32_t), '\0');
  file_crc.Fold(prefix.data(), prefix.size());
  return {};
}

}  // namespace

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

    // Everything past the header is folded into the file checksum as it is written.
    SnapshotSink sink(output_stream);

    // Write config section
    std::ostringstream config_ss;
    auto config_result = SerializeConfig(config_ss, config);
    if (!config_result) {
      return config_result;
    }
    std::string config_data = config_ss.str();
    auto config_write = WriteSizedPayload(sink.tracked, config_data, max_config_size, "config section");
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
      auto stats_write = WriteSizedPayload(sink.tracked, stats_data, max_stats_size, "statistics section");
      if (!stats_write) {
        return stats_write;
      }
    }

    // Write store data section
    uint32_t store_count = metadata_store != nullptr ? 4 : 3;  // events, co_occurrence, vectors, metadata
    WriteBinary(sink.tracked, store_count);

    auto events_write =
        WriteStoreSection(sink, kEventsStoreName, "events", store_stats, max_store_stats_size, max_store_data_size,
                          [&](std::ostream& section) { return SerializeEventStore(section, event_store); });
    if (!events_write) {
      return events_write;
    }

    auto co_occurrence_write = WriteStoreSection(
        sink, kCoOccurrenceStoreName, "co-occurrence", store_stats, max_store_stats_size, max_store_data_size,
        [&](std::ostream& section) { return SerializeCoOccurrenceIndex(section, co_index); });
    if (!co_occurrence_write) {
      return co_occurrence_write;
    }

    auto vectors_write =
        WriteStoreSection(sink, kVectorsStoreName, "vectors", store_stats, max_store_stats_size, max_store_data_size,
                          [&](std::ostream& section) { return SerializeVectorStore(section, vector_store); });
    if (!vectors_write) {
      return vectors_write;
    }

    if (metadata_store != nullptr) {
      auto metadata_write =
          WriteStoreSection(sink, kMetadataStoreName, "metadata", nullptr, max_store_stats_size, max_store_data_size,
                            [&](std::ostream& section) { return SerializeMetadataStore(section, *metadata_store); });
      if (!metadata_write) {
        return metadata_write;
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

    // Rewrite the header with the final total_file_size (file_crc32 still 0 as placeholder)
    header.file_crc32 = 0;
    output_stream.seekp(snapshot_format::kFixedHeaderSize, std::ios::beg);
    auto rewrite_header_result = WriteHeaderV1(output_stream, header);
    if (!rewrite_header_result) {
      return rewrite_header_result;
    }

    // The file CRC32 is the checksum of the final prefix followed by the body folded above.
    std::ostringstream prefix_ss;
    prefix_ss.write(snapshot_format::kMagicNumber.data(), snapshot_format::kMagicNumber.size());
    WriteBinary(prefix_ss, version);
    auto prefix_result = WriteHeaderV1(prefix_ss, header);
    if (!prefix_result) {
      return prefix_result;
    }
    const std::string prefix = prefix_ss.str();
    FileCrc prefix_crc;
    prefix_crc.Fold(prefix.data(), prefix.size());
    prefix_crc.FoldRun(sink.Crc().Value(), sink.Crc().Length());
    header.file_crc32 = prefix_crc.Value();

    output_stream.seekp(static_cast<std::streamoff>(kFileCRC32Offset), std::ios::beg);
    if (!WriteBinary(output_stream, header.file_crc32)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to write snapshot file CRC32"));
    }

    // Close file to flush all data and surface buffered write failures.
    output_stream.close();
    if (output_stream.fail()) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError, "Failed to close snapshot temp file"));
    }

#ifndef _WIN32
    // Persist the same opened inode that was written.
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

namespace {

/// Parse every section of an opened snapshot, folding the bytes read into the file checksum.
Expected<void, Error> ReadSnapshotSections(OpenedSnapshot& opened, const std::string& filepath, config::Config& config,
                                           events::EventStore& event_store, events::CoOccurrenceIndex& co_index,
                                           vectors::VectorStore& vector_store, SnapshotStatistics* stats,
                                           std::unordered_map<std::string, StoreStatistics>* store_stats,
                                           snapshot_format::IntegrityError* integrity_error,
                                           vectors::MetadataStore* metadata_store) {
  std::ifstream& input_stream = opened.stream;
  const HeaderV1& header = opened.header;

  {
    // The size and CRC declarations are checked up front; the CRC value itself
    // is checked once every section has been read, so the file is read once.
    const auto sections_begin = input_stream.tellg();
    auto bounds_result = CheckFileBounds(input_stream, header, integrity_error);
    if (!bounds_result) {
      return bounds_result;
    }
    FileCrc file_crc;
    auto prefix_result = FoldHeaderPrefix(input_stream, sections_begin, file_crc);
    if (!prefix_result) {
      return prefix_result;
    }

    // Read config section
    uint32_t config_size = 0;
    uint32_t config_crc = 0;
    if (!ReadFolded(input_stream, file_crc, config_size)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read config size"));
    }
    if (!ReadFolded(input_stream, file_crc, config_crc)) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read config CRC"));
    }
    auto config_data = ReadCheckedSection(
        input_stream, file_crc, config_size, config_crc,
        {"config section", kMaxConfigSize, snapshot_format::CRCErrorType::ConfigCRC, ""}, integrity_error);
    if (!config_data) {
      return MakeUnexpected(config_data.error());
    }
    std::istringstream config_ss(*config_data);
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
      if (!ReadFolded(input_stream, file_crc, stats_size)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read statistics size"));
      }
      if (!ReadFolded(input_stream, file_crc, stats_crc)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read statistics CRC"));
      }
      auto stats_data = ReadCheckedSection(
          input_stream, file_crc, stats_size, stats_crc,
          {"statistics section", kMaxStatsSize, snapshot_format::CRCErrorType::StatsCRC, ""}, integrity_error);
      if (!stats_data) {
        return MakeUnexpected(stats_data.error());
      }
      if (stats != nullptr) {
        std::istringstream stats_ss(*stats_data);
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
    if (!ReadFolded(input_stream, file_crc, store_count)) {
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
      if (!ReadStringFolded(input_stream, file_crc, store_name)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Failed to read store name"));
      }
      if (store_name != kEventsStoreName && store_name != kCoOccurrenceStoreName && store_name != kVectorsStoreName &&
          store_name != kMetadataStoreName) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError, "Unknown store name: '" + store_name + "'"));
      }
      if (!seen_stores.insert(store_name).second) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Duplicate store section: '" + store_name + "'"));
      }

      // Read store statistics (if present)
      uint32_t store_stats_size = 0;
      if (!ReadFolded(input_stream, file_crc, store_stats_size)) {
        return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                        "Failed to read store statistics size for '" + store_name + "'"));
      }
      if (store_stats_size > 0) {
        uint32_t store_stats_crc = 0;
        if (!ReadFolded(input_stream, file_crc, store_stats_crc)) {
          return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                          "Failed to read store statistics CRC for '" + store_name + "'"));
        }
        auto store_stats_data = ReadCheckedSection(input_stream, file_crc, store_stats_size, store_stats_crc,
                                                   {"store statistics for '" + store_name + "'", kMaxStoreStatsSize,
                                                    snapshot_format::CRCErrorType::StoreStatsCRC, store_name},
                                                   integrity_error);
        if (!store_stats_data) {
          return MakeUnexpected(store_stats_data.error());
        }
        if (store_stats != nullptr) {
          std::istringstream store_stats_ss(*store_stats_data);
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
      if (!ReadFolded(input_stream, file_crc, store_data_size)) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Failed to read store data size for '" + store_name + "'"));
      }
      if (!ReadFolded(input_stream, file_crc, store_data_crc)) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Failed to read store data CRC for '" + store_name + "'"));
      }
      auto store_data = ReadCheckedSection(
          input_stream, file_crc, store_data_size, store_data_crc,
          {"store data for '" + store_name + "'", kMaxStoreDataSize, StoreDataCrcErrorType(store_name), store_name},
          integrity_error);
      if (!store_data) {
        return MakeUnexpected(store_data.error());
      }

      // Parse the bytes that were already read rather than handing them to an
      // istringstream, which would copy the whole section a second time.
      BufferViewStreambuf store_data_buffer(store_data->data(), store_data->size());
      std::istream store_data_ss(&store_data_buffer);

      // Deserialize based on store name
      if (store_name == kEventsStoreName) {
        auto deserialize_result = DeserializeEventStore(store_data_ss, event_store);
        if (!deserialize_result) {
          return deserialize_result;
        }
      } else if (store_name == kCoOccurrenceStoreName) {
        auto deserialize_result = DeserializeCoOccurrenceIndex(store_data_ss, co_index);
        if (!deserialize_result) {
          return deserialize_result;
        }
      } else if (store_name == kVectorsStoreName) {
        auto deserialize_result = DeserializeVectorStore(store_data_ss, vector_store);
        if (!deserialize_result) {
          return deserialize_result;
        }
      } else if (store_name == kMetadataStoreName) {
        auto& metadata_target = metadata_store != nullptr ? *metadata_store : discarded_metadata_store;
        auto deserialize_result = DeserializeMetadataStore(store_data_ss, metadata_target);
        if (!deserialize_result) {
          return deserialize_result;
        }
      }
      if (!IsFullyConsumed(store_data_ss)) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Trailing bytes in store data for '" + store_name + "'"));
      }
    }

    for (const std::string required_store : {kEventsStoreName, kCoOccurrenceStoreName, kVectorsStoreName}) {
      if (seen_stores.count(required_store) == 0) {
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpReadError, "Missing required store: '" + required_store + "'"));
      }
    }
    if (seen_stores.count(kMetadataStoreName) > 0) {
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

    auto crc_result = CheckFileCrc(file_crc.Value(), header, integrity_error);
    if (!crc_result) {
      return crc_result;
    }

    LogStorageInfo("snapshot_read", "Snapshot loaded successfully from " + filepath);
    return {};
  }
}

}  // namespace

Expected<void, Error> ReadSnapshotV1(const std::string& filepath, config::Config& config,
                                     events::EventStore& event_store, events::CoOccurrenceIndex& co_index,
                                     vectors::VectorStore& vector_store, SnapshotStatistics* stats,
                                     std::unordered_map<std::string, StoreStatistics>* store_stats,
                                     snapshot_format::IntegrityError* integrity_error,
                                     vectors::MetadataStore* metadata_store) {
  auto opened = OpenSnapshotWithHeader(filepath);
  if (!opened) {
    return MakeUnexpected(opened.error());
  }
  auto sections = ReadSnapshotSections(*opened, filepath, config, event_store, co_index, vector_store, stats,
                                       store_stats, integrity_error, metadata_store);
  if (sections) {
    return sections;
  }

  // The single pass cannot tell damage from a semantically invalid file, and a
  // damaged file is reported as such ahead of whatever the parse tripped over.
  opened->stream.clear();
  auto integrity = VerifyFileLevelIntegrity(opened->stream, opened->header, integrity_error);
  if (!integrity) {
    return integrity;
  }
  return sections;
}

Expected<void, Error> VerifySnapshotIntegrity(const std::string& filepath,
                                              snapshot_format::IntegrityError& integrity_error) {
  auto opened = OpenSnapshotWithHeader(filepath);
  if (!opened) {
    integrity_error.type = snapshot_format::CRCErrorType::FileCRC;
    integrity_error.message = opened.error().message();
    return MakeUnexpected(opened.error());
  }

  // Verify whole-file size and CRC32 via the shared helper.
  auto file_integrity_result = VerifyFileLevelIntegrity(opened->stream, opened->header, &integrity_error);
  if (!file_integrity_result) {
    return file_integrity_result;
  }

  LogStorageInfo("snapshot_verify", "Snapshot integrity verified: " + filepath);
  return {};
}

Expected<void, Error> GetSnapshotInfo(const std::string& filepath, SnapshotInfo& info) {
  auto opened = OpenSnapshotWithHeader(filepath);
  if (!opened) {
    return MakeUnexpected(opened.error());
  }
  std::ifstream& input_stream = opened->stream;
  const HeaderV1& header = opened->header;

  info.version = opened->version;
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

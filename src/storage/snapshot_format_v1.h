/**
 * @file snapshot_format_v1.h
 * @brief Snapshot file format Version 1 serialization/deserialization
 *
 * Reference: ../mygram-db/src/storage/dump_format_v1.h
 * Reusability: 85% (adapted for nvecd stores: EventStore, CoOccurrenceIndex, VectorStore)
 *
 * This file defines the Version 1 snapshot format for nvecd. Snapshots are binary
 * files (.dmp) that contain the complete database state including configuration,
 * event store, co-occurrence index, and vector store.
 *
 * File Structure:
 * ┌─────────────────────────────────────────────────────────────┐
 * │ Fixed File Header (8 bytes)                                 │
 * │   - Magic: "NVEC" (4 bytes)                                 │
 * │   - Format Version: 1 (4 bytes)                             │
 * ├─────────────────────────────────────────────────────────────┤
 * │ Version 1 Header (variable length)                          │
 * │   - Header Size                                             │
 * │   - Flags (kWithStatistics, kWithCRC)                       │
 * │   - Snapshot Timestamp                                      │
 * │   - Total File Size (for truncation detection)             │
 * │   - File CRC32 (entire file checksum)                      │
 * │   - Reserved (for future extensions)                       │
 * ├─────────────────────────────────────────────────────────────┤
 * │ Config Section                                              │
 * │   - Length (4 bytes)                                        │
 * │   - CRC32 (4 bytes)                                         │
 * │   - Serialized Configuration                               │
 * ├─────────────────────────────────────────────────────────────┤
 * │ Statistics Section (optional, if kWithStatistics)          │
 * │   - Length (4 bytes)                                        │
 * │   - CRC32 (4 bytes)                                         │
 * │   - Snapshot Statistics                                     │
 * ├─────────────────────────────────────────────────────────────┤
 * │ Store Data Section                                          │
 * │   - Store Count (4 bytes): 3 (events, co_occurrence, vectors)│
 * │   ┌───────────────────────────────────────────────────────┐│
 * │   │ For each store:                                       ││
 * │   │   - Store Name (length-prefixed string)               ││
 * │   │   - Store Statistics (optional, if kWithStatistics)  ││
 * │   │   - Store Data (length + CRC32 + data)               ││
 * │   └───────────────────────────────────────────────────────┘│
 * └─────────────────────────────────────────────────────────────┘
 *
 * All multi-byte integers are stored in little-endian format.
 * All strings are UTF-8 encoded with length-prefix (uint32_t).
 * CRC32 checksums use zlib implementation (polynomial: 0xEDB88320).
 */

#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <unordered_map>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "storage/snapshot_format.h"
#include "utils/error.h"
#include "utils/expected.h"
#include "vectors/vector_store.h"

namespace nvecd::storage::snapshot_v1 {

using utils::Error;
using utils::Expected;

/**
 * @brief Version 1 snapshot header
 *
 * This header contains metadata about the snapshot file and integrity
 * verification information. It follows the fixed 8-byte file header.
 *
 * Layout:
 * | Offset | Size | Field             | Description                        |
 * |--------|------|-------------------|------------------------------------|
 * | 0      | 4    | header_size       | Size of V1 header in bytes         |
 * | 4      | 4    | flags             | Feature flags (see flags_v1)       |
 * | 8      | 8    | snapshot_timestamp| Unix timestamp (seconds)           |
 * | 16     | 8    | total_file_size   | Expected file size (bytes)         |
 * | 24     | 4    | file_crc32        | CRC32 of entire file               |
 * | 28     | 4    | reserved_length   | Length of reserved field           |
 * | 32     | N    | reserved          | Reserved for future use            |
 */
struct HeaderV1 {
  uint32_t header_size = 0;         // Size of this header in bytes
  uint32_t flags = 0;               // Flags (see snapshot_format::flags_v1)
  uint64_t snapshot_timestamp = 0;  // Unix timestamp when snapshot was created
  uint64_t total_file_size = 0;     // Expected total file size (for truncation detection)
  uint32_t file_crc32 = 0;          // CRC32 of entire file (excluding this field itself)
  std::string reserved;             // Reserved for future extensions
};

/**
 * @brief Serialize Config to output stream
 * @param output_stream Output stream
 * @param config Configuration to serialize
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> SerializeConfig(std::ostream& output_stream, const config::Config& config);

/**
 * @brief Deserialize Config from input stream
 * @param input_stream Input stream
 * @param config Configuration to deserialize into
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> DeserializeConfig(std::istream& input_stream, config::Config& config);

/**
 * @brief Serialize SnapshotStatistics to output stream
 * @param output_stream Output stream
 * @param stats Statistics to serialize
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> SerializeStatistics(std::ostream& output_stream, const SnapshotStatistics& stats);

/**
 * @brief Deserialize SnapshotStatistics from input stream
 * @param input_stream Input stream
 * @param stats Statistics to deserialize into
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> DeserializeStatistics(std::istream& input_stream, SnapshotStatistics& stats);

/**
 * @brief Serialize StoreStatistics to output stream
 * @param output_stream Output stream
 * @param stats Store statistics to serialize
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> SerializeStoreStatistics(std::ostream& output_stream, const StoreStatistics& stats);

/**
 * @brief Deserialize StoreStatistics from input stream
 * @param input_stream Input stream
 * @param stats Store statistics to deserialize into
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> DeserializeStoreStatistics(std::istream& input_stream, StoreStatistics& stats);

/**
 * @brief Serialize EventStore to output stream
 * @param output_stream Output stream
 * @param event_store EventStore to serialize
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> SerializeEventStore(std::ostream& output_stream, const events::EventStore& event_store);

/**
 * @brief Deserialize EventStore from input stream
 * @param input_stream Input stream
 * @param event_store EventStore to deserialize into
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> DeserializeEventStore(std::istream& input_stream, events::EventStore& event_store);

/**
 * @brief Serialize CoOccurrenceIndex to output stream
 * @param output_stream Output stream
 * @param co_index CoOccurrenceIndex to serialize
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> SerializeCoOccurrenceIndex(std::ostream& output_stream,
                                                  const events::CoOccurrenceIndex& co_index);

/**
 * @brief Deserialize CoOccurrenceIndex from input stream
 * @param input_stream Input stream
 * @param co_index CoOccurrenceIndex to deserialize into
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> DeserializeCoOccurrenceIndex(std::istream& input_stream, events::CoOccurrenceIndex& co_index);

/**
 * @brief Serialize VectorStore to output stream
 * @param output_stream Output stream
 * @param vector_store VectorStore to serialize
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> SerializeVectorStore(std::ostream& output_stream, const vectors::VectorStore& vector_store);

/**
 * @brief Deserialize VectorStore from input stream
 * @param input_stream Input stream
 * @param vector_store VectorStore to deserialize into
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> DeserializeVectorStore(std::istream& input_stream, vectors::VectorStore& vector_store);

/**
 * @brief Write Version 1 snapshot header
 * @param output_stream Output stream
 * @param header Header to write
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> WriteHeaderV1(std::ostream& output_stream, const HeaderV1& header);

/**
 * @brief Read Version 1 snapshot header
 * @param input_stream Input stream
 * @param header Header to read into
 * @return Expected<void, Error> Success or error with details
 */
Expected<void, Error> ReadHeaderV1(std::istream& input_stream, HeaderV1& header);

/**
 * @brief Write complete snapshot to file (Version 1 format)
 *
 * Creates a snapshot file containing the complete database state. The write process
 * is atomic - data is first written to a temporary file, then renamed on success.
 *
 * The snapshot includes:
 * - Fixed file header (magic number "NVEC" + version)
 * - V1 header (metadata, flags, CRC32)
 * - Configuration section
 * - Statistics section (if stats provided)
 * - Store data (EventStore, CoOccurrenceIndex, VectorStore)
 *
 * CRC32 checksums are calculated for:
 * - Entire file (file_crc32 in header)
 * - Config section
 * - Statistics section
 * - Each store's data
 *
 * Write process:
 * 1. Write fixed header (magic + version)
 * 2. Write V1 header (with placeholder CRC32 and file size)
 * 3. Write config section
 * 4. Write statistics section (if provided)
 * 5. Write store data sections
 * 6. Calculate file CRC32 and update header
 * 7. Atomic rename from temp file to final path
 *
 * @param filepath Output file path (e.g., "/var/lib/nvecd/dumps/snapshot.dmp")
 * @param config Full configuration to serialize
 * @param event_store EventStore to serialize
 * @param co_index CoOccurrenceIndex to serialize
 * @param vector_store VectorStore to serialize
 * @param stats Optional snapshot-level statistics
 * @param store_stats Optional per-store statistics map
 * @return Expected<void, Error> Success or error with details (context: filepath)
 *
 * @note Writes to temporary file first (.tmp suffix), then atomic rename
 * @note All data is written in little-endian format
 * @note CRC32 uses zlib implementation (polynomial: 0xEDB88320)
 */
Expected<void, Error> WriteSnapshotV1(const std::string& filepath, const config::Config& config,
                                      const events::EventStore& event_store,
                                      const events::CoOccurrenceIndex& co_index,
                                      const vectors::VectorStore& vector_store,
                                      const SnapshotStatistics* stats = nullptr,
                                      const std::unordered_map<std::string, StoreStatistics>* store_stats = nullptr);

/**
 * @brief Read complete snapshot from file (Version 1 format)
 *
 * Loads a snapshot file and restores the complete database state. All data is
 * validated using CRC32 checksums to ensure integrity.
 *
 * Load process:
 * 1. Read and validate fixed header (magic + version)
 * 2. Validate file size against header.total_file_size
 * 3. Calculate and verify file-level CRC32
 * 4. Read and deserialize config section (verify CRC32)
 * 5. Read statistics section if present (verify CRC32)
 * 6. For each store:
 *    - Read store name
 *    - Read store statistics if present (verify CRC32)
 *    - Load store data (verify CRC32)
 * 7. Populate stores with loaded data
 *
 * Error handling:
 * - Version mismatch: Returns false if version is unsupported
 * - File truncation: Detected via file size check
 * - CRC mismatch: Detected at file and section levels
 * - All errors are logged and optionally returned in integrity_error
 *
 * @param filepath Input file path (e.g., "/var/lib/nvecd/dumps/snapshot.dmp")
 * @param config Output configuration loaded from snapshot
 * @param event_store EventStore to load into (must be pre-allocated)
 * @param co_index CoOccurrenceIndex to load into (must be pre-allocated)
 * @param vector_store VectorStore to load into (must be pre-allocated)
 * @param stats Optional output for snapshot-level statistics
 * @param store_stats Optional output for per-store statistics map
 * @param integrity_error Optional output for detailed integrity error information
 * @return Expected<void, Error> Success or error with details (context: filepath, section)
 *
 * @note All stores MUST be pre-allocated
 * @note All loaded data replaces existing data in the provided objects
 * @note CRC32 verification is always performed
 * @note Little-endian format expected for all multi-byte integers
 */
Expected<void, Error> ReadSnapshotV1(const std::string& filepath, config::Config& config,
                                     events::EventStore& event_store, events::CoOccurrenceIndex& co_index,
                                     vectors::VectorStore& vector_store, SnapshotStatistics* stats = nullptr,
                                     std::unordered_map<std::string, StoreStatistics>* store_stats = nullptr,
                                     snapshot_format::IntegrityError* integrity_error = nullptr);

/**
 * @brief Verify snapshot file integrity without loading
 *
 * Validates a snapshot file's integrity without loading the actual data into memory.
 * This is much faster than a full load and useful for validating backups.
 *
 * Verification checks:
 * 1. File exists and is readable
 * 2. Magic number is correct ("NVEC")
 * 3. Version is supported
 * 4. File size matches header.total_file_size
 * 5. File-level CRC32 matches calculated checksum
 *
 * This function does NOT verify:
 * - Individual section CRC32s (config, stats, stores)
 * - Data deserialization correctness
 * - Configuration validity
 *
 * @param filepath Snapshot file path to verify
 * @param integrity_error Output for detailed error information if verification fails
 * @return Expected<void, Error> Success or error with details (context: filepath)
 *
 * @note This is a fast check suitable for cron jobs and health checks
 * @note For full validation, use ReadSnapshotV1() which verifies all sections
 */
Expected<void, Error> VerifySnapshotIntegrity(const std::string& filepath,
                                               snapshot_format::IntegrityError& integrity_error);

/**
 * @brief Calculate CRC32 checksum for data
 * @param data Data pointer
 * @param length Data length in bytes
 * @return CRC32 checksum
 */
uint32_t CalculateCRC32(const void* data, size_t length);

/**
 * @brief Calculate CRC32 checksum for string
 * @param str String data
 * @return CRC32 checksum
 */
uint32_t CalculateCRC32(const std::string& str);

/**
 * @brief Snapshot file metadata information
 *
 * Lightweight metadata structure returned by GetSnapshotInfo().
 * Contains summary information about a snapshot file without loading
 * the actual data.
 */
struct SnapshotInfo {
  uint32_t version = 0;          // Format version (1 for V1)
  uint32_t store_count = 0;      // Number of stores in snapshot (should be 3)
  uint32_t flags = 0;            // Feature flags (see snapshot_format::flags_v1)
  uint64_t file_size = 0;        // Total file size in bytes
  uint64_t timestamp = 0;        // Unix timestamp when snapshot was created
  bool has_statistics = false;   // True if snapshot contains statistics sections
};

/**
 * @brief Read snapshot file metadata without loading data
 *
 * Quickly reads snapshot metadata without loading stores.
 * Useful for displaying snapshot information to users (DUMP INFO command).
 *
 * Information extracted:
 * - Format version
 * - Store count (should be 3: events, co_occurrence, vectors)
 * - Feature flags (statistics, CRC, etc.)
 * - File size
 * - Creation timestamp
 *
 * This function:
 * - Does NOT load store data
 * - Does NOT verify CRC checksums
 * - Only reads headers and config section
 * - Is very fast (< 1ms for typical files)
 *
 * @param filepath Snapshot file path
 * @param info Output structure for snapshot metadata
 * @return Expected<void, Error> Success or error with details (context: filepath)
 *
 * @note This does not validate file integrity - use VerifySnapshotIntegrity() for that
 * @note Suitable for listing/browsing snapshot files
 */
Expected<void, Error> GetSnapshotInfo(const std::string& filepath, SnapshotInfo& info);

}  // namespace nvecd::storage::snapshot_v1

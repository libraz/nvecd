/**
 * @file snapshot_format.h
 * @brief Binary format definitions for snapshot files (.dmp)
 *
 * Reference: ../mygram-db/src/storage/dump_format.h
 * Reusability: 90% (adapted for nvecd stores instead of tables)
 *
 * This file defines constants and data structures for nvecd snapshot files.
 * Snapshots are binary files that contain the complete database state including
 * configuration, event store, co-occurrence index, and vector store.
 *
 * File Format Overview:
 * Every snapshot file starts with an 8-byte fixed header:
 *   - 4 bytes: Magic number "NVEC" (0x4E564543)
 *   - 4 bytes: Format version (uint32_t, little-endian)
 *
 * The fixed header is followed by version-specific data.
 * See snapshot_format_v1.h for Version 1 format details.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace nvecd::storage {

/**
 * @brief Snapshot file format constants
 */
namespace snapshot_format {

// Magic number for snapshot files ("NVEC" in ASCII)
// Used to quickly identify nvecd snapshot files
constexpr std::array<char, 4> kMagicNumber = {'N', 'V', 'E', 'C'};

// Current format version (version we write)
// Increment when introducing breaking changes to the format
constexpr uint32_t kCurrentVersion = 1;

// Maximum supported version (versions we can read)
// Must be >= kCurrentVersion, can support newer versions for forward compatibility
constexpr uint32_t kMaxSupportedVersion = 1;

// Minimum supported version (oldest version we can read)
// Must be <= kCurrentVersion, set to 1 to support all versions since initial release
constexpr uint32_t kMinSupportedVersion = 1;

// Fixed file header size (magic + version)
// This header is present in all snapshot versions
constexpr size_t kFixedHeaderSize = 8;

/**
 * @brief Format version enum for type safety
 */
// NOLINTNEXTLINE(performance-enum-size) - Must match file format uint32_t
enum class FormatVersion : uint32_t {
  V1 = 1,  // Initial version
  // Future versions can be added here
  // V2 = 2,
  // V3 = 3,
};

/**
 * @brief Flags for future extensions (Version 1)
 *
 * These flags are stored in the V1 header and indicate which features
 * are enabled for a particular snapshot file. Multiple flags can be
 * combined using bitwise OR.
 *
 * Current flags:
 * - kWithStatistics: Snapshot includes performance statistics
 * - kWithCRC: Snapshot includes CRC32 checksums (always set in V1)
 *
 * Reserved flags for future use:
 * - kCompressed: Data compression (not yet implemented)
 * - kEncrypted: Data encryption (not yet implemented)
 * - kIncremental: Incremental snapshot (not yet implemented)
 */
namespace flags_v1 {
constexpr uint32_t kNone = 0x00000000;            // No flags set
constexpr uint32_t kCompressed = 0x00000001;      // Data is compressed (reserved for future)
constexpr uint32_t kEncrypted = 0x00000002;       // Data is encrypted (reserved for future)
constexpr uint32_t kIncremental = 0x00000004;     // Incremental snapshot (reserved for future)
constexpr uint32_t kWithStatistics = 0x00000008;  // Contains statistics sections
constexpr uint32_t kWithCRC = 0x00000010;         // Contains CRC checksums (always set in V1)
}  // namespace flags_v1

/**
 * @brief CRC error types
 *
 * Classifies the type of CRC mismatch detected during snapshot verification.
 * This helps identify which part of the snapshot file is corrupted.
 */
enum class CRCErrorType : std::uint8_t {
  None = 0,             // No error detected
  FileCRC = 1,          // File-level CRC mismatch (entire file corrupted)
  ConfigCRC = 2,        // Config section CRC mismatch
  StatsCRC = 3,         // Statistics section CRC mismatch
  StoreStatsCRC = 4,    // Store statistics CRC mismatch (store-specific)
  EventStoreCRC = 5,    // EventStore data CRC mismatch
  CoOccurrenceCRC = 6,  // CoOccurrenceIndex data CRC mismatch
  VectorStoreCRC = 7,   // VectorStore data CRC mismatch
};

/**
 * @brief File integrity error information
 *
 * Contains detailed information about integrity check failures.
 * Returned by VerifySnapshotIntegrity() and ReadSnapshotV1().
 *
 * Fields:
 * - type: Type of CRC error (None if no error)
 * - message: Human-readable error description
 * - store_name: Store name (for store-specific errors), empty otherwise
 */
struct IntegrityError {
  CRCErrorType type = CRCErrorType::None;  // Type of error detected
  std::string message;                     // Human-readable error message
  std::string store_name;                  // Store name (for store-specific errors)

  /**
   * @brief Check if an error occurred
   * @return true if type != None
   */
  [[nodiscard]] bool HasError() const { return type != CRCErrorType::None; }
};

}  // namespace snapshot_format

/**
 * @brief Snapshot statistics (stored in snapshot file)
 *
 * Aggregate statistics across all stores in the snapshot.
 * Only included when kWithStatistics flag is set.
 *
 * Use cases:
 * - Monitoring snapshot growth over time
 * - Capacity planning
 * - Performance analysis
 * - Backup validation
 */
struct SnapshotStatistics {
  uint64_t total_contexts = 0;        // Total contexts in EventStore
  uint64_t total_events = 0;          // Total events across all contexts
  uint64_t total_co_occurrences = 0;  // Total co-occurrence entries
  uint64_t total_vectors = 0;         // Total vectors stored
  uint64_t total_bytes = 0;           // Total memory usage (bytes)
  uint64_t snapshot_time_ms = 0;      // Time taken to create snapshot (milliseconds)
};

/**
 * @brief Per-store statistics (stored in snapshot file)
 *
 * Statistics for a single store. Only included when kWithStatistics flag is set.
 *
 * Fields:
 * - item_count: Number of items in the store (contexts, co-occurrences, vectors)
 * - memory_bytes: Memory used by the store (bytes)
 * - last_update_time: Unix timestamp of last update
 */
struct StoreStatistics {
  uint64_t item_count = 0;        // Number of items in store
  uint64_t memory_bytes = 0;      // Memory usage (bytes)
  uint64_t last_update_time = 0;  // Last update timestamp (Unix time, seconds)
};

}  // namespace nvecd::storage

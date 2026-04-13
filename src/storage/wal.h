/**
 * @file wal.h
 * @brief Lightweight Write-Ahead Log for crash recovery
 *
 * Records operation logs (VECSET, EVENT, META) to enable recovery of
 * operations lost between snapshots. Vector bodies are optionally included.
 *
 * File format (per record):
 *   [length:u32] [crc32:u32] [sequence:u64] [timestamp_us:u64] [op:u8] [payload...]
 *
 * File naming: wal-NNNNNN.log (6-digit zero-padded sequence)
 * Rotation: at max_file_size bytes per file
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::storage {

using utils::Error;
using utils::Expected;

/// WAL record header size: sequence(8) + timestamp_us(8) + op(1) = 17 bytes
static constexpr uint32_t kWalRecordHeaderSize = 17;

/// WAL file header: magic "NWAL" (4) + version (4) = 8 bytes
static constexpr uint32_t kWalFileHeaderSize = 8;

/// WAL magic number
static constexpr uint32_t kWalMagic = 0x4C41574E;  // "NWAL" in little-endian

/// WAL format version
static constexpr uint32_t kWalVersion = 1;

/**
 * @brief WAL operation types
 */
enum class WalOpType : uint8_t {
  kVecSet = 1,    ///< Vector registration
  kVecDel = 2,    ///< Vector deletion
  kEventAdd = 3,  ///< Event addition
  kEventDel = 4,  ///< Event deletion
  kMetaSet = 5,   ///< Metadata set
  kMetaDel = 6,   ///< Metadata deletion
};

/**
 * @brief WAL record (deserialized)
 */
struct WalRecord {
  uint64_t sequence = 0;      ///< Monotonically increasing sequence number
  uint64_t timestamp_us = 0;  ///< Microsecond timestamp
  WalOpType op = WalOpType::kVecSet;
  std::vector<uint8_t> payload;  ///< Variable-length payload
};

/**
 * @brief Lightweight Write-Ahead Log
 *
 * Provides crash recovery by recording operations to disk before execution.
 * Supports file rotation, CRC32 integrity checking, and configurable fsync.
 */
class WriteAheadLog {
 public:
  struct Config {
    std::string directory;
    uint64_t max_file_size = 64ULL * 1024 * 1024;  ///< 64MB per file
    bool sync_on_write = false;                    ///< fsync every write (high durability)
    uint32_t sync_interval_ms = 100;               ///< Batch fsync interval
    bool include_vectors = true;                   ///< Include vector bodies in VECSET
  };

  WriteAheadLog() = default;
  ~WriteAheadLog();

  // Non-copyable, non-movable
  WriteAheadLog(const WriteAheadLog&) = delete;
  WriteAheadLog& operator=(const WriteAheadLog&) = delete;
  WriteAheadLog(WriteAheadLog&&) = delete;
  WriteAheadLog& operator=(WriteAheadLog&&) = delete;

  /**
   * @brief Open WAL directory, recover state from existing files
   * @param config WAL configuration
   * @return Success or error
   */
  Expected<void, Error> Open(const Config& config);

  /**
   * @brief Close WAL, flush pending writes, stop sync thread
   */
  void Close();

  /**
   * @brief Append an operation record to the WAL
   * @param op Operation type
   * @param payload Serialized payload
   * @param payload_size Payload size in bytes
   * @return Sequence number of the appended record, or error
   */
  Expected<uint64_t, Error> Append(WalOpType op, const void* payload, uint32_t payload_size);

  /**
   * @brief Replay records from a given sequence number
   * @param from_sequence Replay records with sequence >= this value
   * @param callback Called for each replayed record
   * @return Number of records replayed, or error
   */
  Expected<uint64_t, Error> Replay(uint64_t from_sequence, const std::function<void(const WalRecord&)>& callback) const;

  /**
   * @brief Delete WAL files containing only records up to the given sequence
   * @param up_to_sequence Delete files where max sequence <= this value
   * @return Success or error
   */
  Expected<void, Error> Truncate(uint64_t up_to_sequence);

  /**
   * @brief Get the current (latest) sequence number
   */
  uint64_t CurrentSequence() const;

  /**
   * @brief Check if WAL is open
   */
  bool IsOpen() const;

 private:
  /// WAL file metadata
  struct WalFile {
    std::string path;
    uint32_t file_number = 0;
    uint64_t min_sequence = 0;
    uint64_t max_sequence = 0;
    uint64_t file_size = 0;
  };

  /// Create a new WAL file for writing
  Expected<void, Error> RotateFile();

  /// Write file header to the current fd
  Expected<void, Error> WriteFileHeader();

  /// Read and validate file header
  static Expected<void, Error> ValidateFileHeader(int fd, const std::string& path);

  /// Scan existing WAL files in the directory
  Expected<void, Error> ScanExistingFiles();

  /// Build the WAL file path from a file number
  std::string MakeFilePath(uint32_t file_number) const;

  /// Background sync thread loop
  void SyncLoop();

  Config config_;
  mutable std::mutex mutex_;
  int current_fd_ = -1;
  uint64_t current_sequence_ = 0;
  uint64_t current_file_size_ = 0;
  uint32_t current_file_number_ = 0;
  std::vector<WalFile> files_;
  bool open_ = false;

  // Batch fsync
  std::thread sync_thread_;
  std::atomic<bool> sync_running_{false};
  std::atomic<bool> needs_sync_{false};
  std::condition_variable sync_cv_;
  std::mutex sync_mutex_;
};

}  // namespace nvecd::storage

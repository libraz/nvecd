/**
 * @file wal.cpp
 * @brief Lightweight Write-Ahead Log implementation
 */

#include "storage/wal.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>

#include "storage/fd_io.h"
#include "storage/snapshot_format_v1.h"
#include "utils/path_utils.h"
#include "utils/structured_log.h"

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
constexpr int kWalNoFollowFlag = 0;
constexpr bool kWalNeedsPathIdentityCheck = true;
#else
constexpr int kWalNoFollowFlag = O_NOFOLLOW;
constexpr bool kWalNeedsPathIdentityCheck = false;
#endif

namespace nvecd::storage {

namespace {

/// Get current time in microseconds
uint64_t NowMicros() {
  auto now = std::chrono::system_clock::now();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

/// WAL segment file name parts: "wal-NNNNNN.log".
constexpr std::string_view kSegmentPrefix = "wal-";
constexpr std::string_view kSegmentSuffix = ".log";

/// Number of digits in a WAL segment file name.
constexpr int kFileNumberDigits = 6;

/// Highest segment number that still fits kFileNumberDigits. A larger number
/// would widen the file name, and the reader matches a fixed-width name only,
/// so such a segment would be invisible to recovery and to truncation.
constexpr uint32_t kMaxFileNumber = 999999;

/// Length of a WAL segment file name: prefix + digits + suffix.
constexpr size_t kFileNameLength = kSegmentPrefix.size() + kFileNumberDigits + kSegmentSuffix.size();

/// A torn tail is repairable only on the newest segment: cut it off and make the cut durable.
Expected<void, Error> RepairTornTail(bool latest_segment, int fd, off_t length, const std::string& path,
                                     const char* what) {
  const std::string message = std::string(what) + " in segment: " + path;
  if (!latest_segment) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kWalCorrupted, message));
  }
  if (::ftruncate(fd, length) != 0 || ::fsync(fd) != 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalCorrupted, message + " (repair failed: " + std::strerror(errno) + ")"));
  }
  return {};
}

/// Open a WAL segment without following links or blocking on special files,
/// then validate the opened inode before any reads or recovery truncation.
Expected<int, Error> OpenWalSegment(const std::string& path, int access_flags, struct stat& file_info) {
  struct stat path_info {};
  if (kWalNeedsPathIdentityCheck &&
      (::lstat(path.c_str(), &path_info) != 0 || !S_ISREG(path_info.st_mode) || path_info.st_size < 0)) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalReadError, "WAL segment path is not a regular file: " + path, path));
  }

  const int fd = ::open(path.c_str(), access_flags | kWalNoFollowFlag | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalReadError,
                         "Failed to open WAL segment: " + path + ": " + std::string(std::strerror(errno)), path));
  }
  if (::fstat(fd, &file_info) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalReadError,
                         "Failed to stat WAL segment: " + path + ": " + std::string(std::strerror(saved_errno)), path));
  }
  if (!S_ISREG(file_info.st_mode) || file_info.st_size < 0) {
    ::close(fd);
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kWalReadError, "WAL segment is not a regular file with a valid size: " + path, path));
  }
  if (kWalNeedsPathIdentityCheck && (file_info.st_dev != path_info.st_dev || file_info.st_ino != path_info.st_ino)) {
    ::close(fd);
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalReadError, "WAL segment path changed while opening: " + path, path));
  }
  return fd;
}

}  // namespace

// ---------------------------------------------------------------------------
// WriteAheadLog
// ---------------------------------------------------------------------------

WriteAheadLog::~WriteAheadLog() {
  Close();
}

Expected<void, Error> WriteAheadLog::Open(const Config& config, uint64_t sequence_floor) {
  // Close owns mutex_ while releasing the active fd. Calling it after taking
  // mutex_ here deadlocks when Open is used to rotate to a new configuration.
  // Lifecycle calls are serialized by the server; close first, then acquire
  // the data-path lock for initialization.
  Close();
  std::lock_guard<std::mutex> lock(mutex_);

  config_ = config;

  // A zero batch interval leaves nobody to consume needs_sync_: the periodic
  // sync thread never starts, so an accepted record would only reach the platter
  // when a segment happens to rotate or the process exits cleanly. Treat it as
  // "fsync on every append" so every schema-valid combination of sync_on_write
  // and sync_interval_ms still makes accepted records durable in bounded time.
  if (!config_.sync_on_write && config_.sync_interval_ms == 0) {
    config_.sync_on_write = true;
  }

  // Ensure directory exists
  if (::mkdir(config_.directory.c_str(), 0700) != 0 && errno != EEXIST) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalWriteError,
                         "Failed to create WAL directory: " + std::string(std::strerror(errno)), config_.directory));
  }
  auto directory_valid = utils::ValidatePrivateDirectory(config_.directory);
  if (!directory_valid) {
    return utils::MakeUnexpected(directory_valid.error());
  }
  std::error_code canonical_error;
  const auto canonical_directory = std::filesystem::canonical(config_.directory, canonical_error);
  if (canonical_error) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kWalWriteError,
                                                  "Failed to resolve WAL directory: " + canonical_error.message(),
                                                  config_.directory));
  }
  config_.directory = canonical_directory.string();

  // Scan existing files to recover state
  auto scan_result = ScanExistingFiles();
  if (!scan_result) {
    return utils::MakeUnexpected(scan_result.error());
  }

  // A previous clean Open/Close without appends leaves a header-only segment.
  // Keeping it forever makes repeated restarts leak one WAL file each time;
  // it has no recovery value, so discard it before creating the next segment.
  bool removed_empty_segment = false;
  std::vector<WalFile> nonempty_files;
  nonempty_files.reserve(files_.size());
  for (auto& file : files_) {
    if (file.max_sequence != 0) {
      nonempty_files.push_back(std::move(file));
      continue;
    }
    if (::unlink(file.path.c_str()) != 0) {
      return utils::MakeUnexpected(utils::MakeError(
          utils::ErrorCode::kWalTruncateFailed,
          "Failed to remove empty WAL segment '" + file.path + "': " + std::string(std::strerror(errno))));
    }
    removed_empty_segment = true;
  }
  files_ = std::move(nonempty_files);
  if (removed_empty_segment && !fd_io::FsyncDirectory(config_.directory)) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kWalTruncateFailed,
        "Failed to fsync WAL directory after removing empty segments: " + std::string(std::strerror(errno))));
  }

  // Truncation removes only segments wholly at or below a checkpoint and never
  // the current one, so a surviving log ends at or above the checkpoint.
  if (current_sequence_ != 0 && current_sequence_ < sequence_floor) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalCorrupted, "WAL ends at sequence " + std::to_string(current_sequence_) +
                                                              ", below the loaded snapshot checkpoint " +
                                                              std::to_string(sequence_floor)));
  }
  current_sequence_ = std::max(current_sequence_, sequence_floor);

  // Open or create the current file
  auto rotate_result = RotateFile();
  if (!rotate_result) {
    return utils::MakeUnexpected(rotate_result.error());
  }

  open_ = true;

  // Start batch fsync thread if not sync_on_write
  if (!config_.sync_on_write && config_.sync_interval_ms > 0) {
    sync_running_.store(true);
    sync_thread_ = std::thread(&WriteAheadLog::SyncLoop, this);
  }

  utils::StructuredLog()
      .Event("wal_opened")
      .Field("directory", config_.directory)
      .Field("files", static_cast<int64_t>(files_.size()))
      .Field("sequence", static_cast<int64_t>(current_sequence_))
      .Info();

  return {};
}

void WriteAheadLog::Close() {
  // Stop sync thread. The stop flag is published under sync_mutex_ so it cannot
  // be stored between the sync thread's predicate check and its wait: a lost
  // wakeup there would stall shutdown for a whole sync_interval_ms.
  if (sync_running_.load()) {
    {
      std::lock_guard<std::mutex> sync_lock(sync_mutex_);
      sync_running_.store(false);
    }
    sync_cv_.notify_all();
    if (sync_thread_.joinable()) {
      sync_thread_.join();
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (current_fd_ >= 0) {
    if (::fsync(current_fd_) != 0) {
      utils::StructuredLog().Event("wal_close_fsync_failed").Field("error", std::string(std::strerror(errno))).Error();
    }
    if (::close(current_fd_) != 0) {
      utils::StructuredLog().Event("wal_close_failed").Field("error", std::string(std::strerror(errno))).Error();
    }
    current_fd_ = -1;
  }
  open_ = false;
}

Expected<uint64_t, Error> WriteAheadLog::Append(WalOpType op, const void* payload, uint32_t payload_size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_ || current_fd_ < 0) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kWalNotOpen, "WAL is not open"));
  }
  if (current_sequence_ == std::numeric_limits<uint64_t>::max()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalWriteError, "WAL sequence number space exhausted"));
  }
  if (payload_size > kMaxWalRecordBodySize - kWalRecordHeaderSize) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalWriteError, "WAL payload exceeds maximum record size"));
  }

  // Check if rotation is needed
  uint32_t record_body_size = kWalRecordHeaderSize + payload_size;
  uint32_t total_record_size = sizeof(uint32_t) + sizeof(uint32_t) + record_body_size;
  // length(4) + crc32(4) + record_header(17) + payload

  if (current_file_size_ + total_record_size > config_.max_file_size && current_file_size_ > kWalFileHeaderSize) {
    // Finalize current file info
    if (!files_.empty()) {
      files_.back().max_sequence = current_sequence_;
    }
    auto result = RotateFile();
    if (!result) {
      return utils::MakeUnexpected(result.error());
    }
  }

  ++current_sequence_;
  uint64_t seq = current_sequence_;
  uint64_t timestamp = NowMicros();

  // Build record body: sequence(8) + timestamp(8) + op(1) + payload
  std::vector<uint8_t> body(record_body_size);
  fd_io::WriteU64(body.data(), seq);
  fd_io::WriteU64(body.data() + 8, timestamp);
  body[16] = static_cast<uint8_t>(op);
  if (payload_size > 0 && payload != nullptr) {
    std::memcpy(body.data() + kWalRecordHeaderSize, payload, payload_size);
  }

  // Calculate CRC32 of the body
  uint32_t crc = snapshot_v1::CalculateCRC32(body.data(), body.size());

  // Write: [length:u32] [crc32:u32] [body...]
  uint8_t header[8];
  fd_io::WriteU32(header, record_body_size);
  fd_io::WriteU32(header + 4, crc);

  auto poison_current_segment = [this](const std::string& message) -> Expected<uint64_t, Error> {
    // A short/failed write leaves an untrusted tail. Never append another ACKed
    // record to that segment: recovery stops at its corrupt tail and would
    // otherwise lose every later record in the same file.
    if (current_fd_ >= 0) {
      ::close(current_fd_);
      current_fd_ = -1;
    }
    open_ = false;
    utils::StructuredLog().Event("wal_segment_poisoned").Field("error", message).Error();
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kWalWriteError, message));
  };

  if (!fd_io::WriteAll(current_fd_, header, sizeof(header))) {
    return poison_current_segment("Failed to write record header: " + std::string(std::strerror(errno)));
  }
  if (!fd_io::WriteAll(current_fd_, body.data(), body.size())) {
    return poison_current_segment("Failed to write record body: " + std::string(std::strerror(errno)));
  }

  current_file_size_ += total_record_size;

  // Update file metadata
  if (!files_.empty()) {
    files_.back().max_sequence = seq;
  }

  if (config_.sync_on_write) {
    if (::fsync(current_fd_) != 0) {
      return poison_current_segment("Failed to fsync WAL record: " + std::string(std::strerror(errno)));
    }
  } else {
    needs_sync_.store(true);
  }

  return seq;
}

Expected<uint64_t, Error> WriteAheadLog::ReplayValidated(
    uint64_t from_sequence, const std::function<Expected<void, Error>(const WalRecord&)>& callback) const {
  std::lock_guard<std::mutex> lock(mutex_);

  uint64_t count = 0;
  bool replay_floor_seen = from_sequence == 0;

  // Collect relevant file paths (sorted by file number)
  std::vector<std::string> paths;
  for (const auto& f : files_) {
    // Include file if it might contain records >= from_sequence
    if (f.max_sequence >= from_sequence || f.max_sequence == 0) {
      paths.push_back(f.path);
    }
  }

  std::optional<uint64_t> previous_sequence;
  for (size_t path_index = 0; path_index < paths.size(); ++path_index) {
    const auto& path = paths[path_index];
    const bool latest_segment = path_index + 1 == paths.size();
    struct stat file_info {};
    auto opened = OpenWalSegment(path, O_RDONLY, file_info);
    if (!opened) {
      return utils::MakeUnexpected(opened.error());
    }
    const int fd = *opened;

    // Validate file header
    auto header_result = ValidateFileHeader(fd, path);
    if (!header_result) {
      ::close(fd);
      return utils::MakeUnexpected(header_result.error());
    }

    // Read records
    while (true) {
      // Read length + crc
      uint8_t rec_header[8];
      const off_t record_offset = ::lseek(fd, 0, SEEK_CUR);
      if (record_offset < 0 || record_offset > file_info.st_size) {
        const auto error = utils::MakeError(utils::ErrorCode::kWalReadError, "Invalid offset in WAL segment: " + path);
        ::close(fd);
        return utils::MakeUnexpected(error);
      }
      const auto remaining_file_bytes = static_cast<uint64_t>(file_info.st_size - record_offset);
      if (remaining_file_bytes == 0) {
        break;
      }
      if (remaining_file_bytes < sizeof(rec_header)) {
        ::close(fd);
        if (latest_segment) {
          return count;  // Recoverable torn header at newest segment tail.
        }
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kStorageCorrupted, "Torn WAL header in non-latest segment: " + path));
      }
      if (!fd_io::ReadAll(fd, rec_header, sizeof(rec_header))) {
        const auto error = utils::MakeError(utils::ErrorCode::kWalReadError, "Failed to read WAL record header");
        ::close(fd);
        return utils::MakeUnexpected(error);
      }

      uint32_t body_length = fd_io::ReadU32(rec_header);
      uint32_t expected_crc = fd_io::ReadU32(rec_header + 4);

      if (body_length < kWalRecordHeaderSize || body_length > kMaxWalRecordBodySize) {
        utils::StructuredLog()
            .Event("wal_record_length_invalid")
            .Field("file", path)
            .Field("body_length", static_cast<int64_t>(body_length))
            .Warn();
        ::close(fd);
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kStorageCorrupted, "Invalid WAL record length in " + path));
      }

      // Read body
      if (remaining_file_bytes - sizeof(rec_header) < body_length) {
        ::close(fd);
        if (latest_segment) {
          return count;  // Recoverable torn body at newest segment tail.
        }
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kStorageCorrupted, "Torn WAL body in non-latest segment: " + path));
      }
      std::vector<uint8_t> body(body_length);
      if (!fd_io::ReadAll(fd, body.data(), body_length)) {
        const auto error = utils::MakeError(utils::ErrorCode::kWalReadError, "Failed to read WAL record body");
        ::close(fd);
        return utils::MakeUnexpected(error);
      }

      // Verify CRC
      uint32_t actual_crc = snapshot_v1::CalculateCRC32(body.data(), body.size());
      if (actual_crc != expected_crc) {
        // CRC mismatch marks the start of a corrupt/torn tail (e.g. a partial
        // write at crash time). The length prefix of this record is itself
        // untrustworthy, so stop replaying this file rather than reinterpreting
        // the remaining bytes as further records.
        utils::StructuredLog()
            .Event("wal_crc_mismatch")
            .Field("file", path)
            .Field("expected_crc", static_cast<int64_t>(expected_crc))
            .Field("actual_crc", static_cast<int64_t>(actual_crc))
            .Warn();
        ::close(fd);
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kWalCRCMismatch, "WAL record CRC mismatch in " + path));
      }

      // Parse record
      WalRecord record;
      record.sequence = fd_io::ReadU64(body.data());
      record.timestamp_us = fd_io::ReadU64(body.data() + 8);
      record.op = static_cast<WalOpType>(body[16]);

      if (record.sequence == 0) {
        ::close(fd);
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kStorageCorrupted, "WAL sequence zero is invalid in " + path));
      }

      if (previous_sequence.has_value() && (previous_sequence.value() == std::numeric_limits<uint64_t>::max() ||
                                            record.sequence != previous_sequence.value() + 1)) {
        ::close(fd);
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kStorageCorrupted,
                             "WAL sequence is not contiguous: previous=" + std::to_string(*previous_sequence) +
                                 ", current=" + std::to_string(record.sequence)));
      }
      previous_sequence = record.sequence;

      if (record.sequence == from_sequence) {
        replay_floor_seen = true;
      }

      if (record.sequence < from_sequence) {
        continue;  // Skip records before requested sequence
      }

      uint32_t payload_size = body_length - kWalRecordHeaderSize;
      if (payload_size > 0) {
        record.payload.assign(body.begin() + kWalRecordHeaderSize, body.end());
      }

      auto applied = callback(record);
      if (!applied) {
        ::close(fd);
        return utils::MakeUnexpected(applied.error());
      }
      ++count;
    }

    ::close(fd);
  }

  if (!replay_floor_seen && from_sequence <= current_sequence_) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalCorrupted,
                         "WAL does not contain requested replay floor sequence " + std::to_string(from_sequence)));
  }

  return count;
}

Expected<void, Error> WriteAheadLog::Truncate(uint64_t up_to_sequence) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<WalFile> remaining;
  remaining.reserve(files_.size());
  uint64_t deleted_count = 0;
  std::optional<Error> unlink_error;

  for (auto& f : files_) {
    // Delete files where ALL records are <= up_to_sequence
    // (i.e., max_sequence <= up_to_sequence AND it's not the current file)
    const bool deletable =
        f.max_sequence <= up_to_sequence && f.max_sequence > 0 && f.file_number != current_file_number_;
    // Stop deleting at the first failure. files_ is ordered by segment number,
    // so the deleted set stays a prefix and the surviving set stays a single
    // contiguous range. Continuing past a failure would leave a gap that the
    // next Open() rejects, making the server unstartable until the directory is
    // repaired by hand.
    if (!deletable || unlink_error.has_value()) {
      remaining.push_back(std::move(f));
      continue;
    }
    if (::unlink(f.path.c_str()) == 0) {
      ++deleted_count;
      continue;
    }
    const int saved_errno = errno;
    remaining.push_back(std::move(f));
    unlink_error = utils::MakeError(
        utils::ErrorCode::kWalTruncateFailed,
        "Failed to unlink WAL segment '" + remaining.back().path + "': " + std::string(std::strerror(saved_errno)));
  }

  files_ = std::move(remaining);

  if (deleted_count > 0 && !fd_io::FsyncDirectory(config_.directory)) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalTruncateFailed,
                         "Failed to fsync WAL directory after truncate: " + std::string(std::strerror(errno))));
  }

  if (deleted_count > 0) {
    utils::StructuredLog()
        .Event("wal_truncated")
        .Field("deleted_files", static_cast<int64_t>(deleted_count))
        .Field("up_to_sequence", static_cast<int64_t>(up_to_sequence))
        .Field("remaining_files", static_cast<int64_t>(files_.size()))
        .Info();
  }

  if (unlink_error.has_value()) {
    return utils::MakeUnexpected(*unlink_error);
  }

  return {};
}

uint64_t WriteAheadLog::CurrentSequence() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_sequence_;
}

bool WriteAheadLog::IsOpen() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return open_;
}

// ---------------------------------------------------------------------------
// Private methods
// ---------------------------------------------------------------------------

Expected<void, Error> WriteAheadLog::RotateFile() {
  // Close current file if open
  if (current_fd_ >= 0) {
    // After a failed fsync the segment's durability is unknown: drop it so a
    // retry cannot succeed against state the kernel may already have discarded.
    const int old_fd = current_fd_;
    current_fd_ = -1;
    if (::fsync(old_fd) != 0) {
      const int saved_errno = errno;
      ::close(old_fd);
      open_ = false;
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kWalRotationFailed,
                           "Failed to fsync WAL segment before rotation: " + std::string(std::strerror(saved_errno))));
    }
    if (::close(old_fd) != 0) {
      open_ = false;
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kWalRotationFailed,
                           "Failed to close WAL segment before rotation: " + std::string(std::strerror(errno))));
    }
  }

  std::string path;
  int fd = -1;
  do {
    // Fail closed at the widest name the reader recognises. A wider name would
    // be written but never scanned back, so the segment would vanish from
    // recovery and stay unreclaimable by Truncate() — a silent data loss that
    // reports no error at all.
    if (current_file_number_ >= kMaxFileNumber) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kWalRotationFailed, "WAL segment number space exhausted"));
    }
    ++current_file_number_;
    path = MakeFilePath(current_file_number_);
    fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | kWalNoFollowFlag | O_CLOEXEC, 0600);
  } while (fd < 0 && errno == EEXIST);

  if (fd < 0) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kWalRotationFailed, "Failed to create WAL file: " + std::string(std::strerror(errno)), path));
  }

  current_fd_ = fd;
  current_file_size_ = 0;

  auto result = WriteFileHeader();
  if (!result) {
    ::close(current_fd_);
    current_fd_ = -1;
    ::unlink(path.c_str());
    return utils::MakeUnexpected(result.error());
  }
  if (::fsync(current_fd_) != 0) {
    const int saved_errno = errno;
    ::close(current_fd_);
    current_fd_ = -1;
    ::unlink(path.c_str());
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalRotationFailed,
                         "Failed to fsync new WAL segment: " + std::string(std::strerror(saved_errno))));
  }
  if (!fd_io::FsyncDirectory(config_.directory)) {
    const int saved_errno = errno;
    ::close(current_fd_);
    current_fd_ = -1;
    ::unlink(path.c_str());
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalRotationFailed,
                         "Failed to fsync WAL directory: " + std::string(std::strerror(saved_errno))));
  }

  // Add file to the list
  WalFile wf;
  wf.path = path;
  wf.file_number = current_file_number_;
  wf.max_sequence = 0;
  files_.push_back(std::move(wf));

  return {};
}

Expected<void, Error> WriteAheadLog::WriteFileHeader() {
  uint8_t header[kWalFileHeaderSize];
  fd_io::WriteU32(header, kWalMagic);
  fd_io::WriteU32(header + 4, kWalVersion);

  if (!fd_io::WriteAll(current_fd_, header, sizeof(header))) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kWalWriteError, "Failed to write WAL file header"));
  }

  current_file_size_ = kWalFileHeaderSize;
  return {};
}

Expected<void, Error> WriteAheadLog::ValidateFileHeader(int fd, const std::string& path) {
  uint8_t header[kWalFileHeaderSize];
  if (!fd_io::ReadAll(fd, header, sizeof(header))) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalReadError, "Failed to read WAL file header", path));
  }

  uint32_t magic = fd_io::ReadU32(header);
  uint32_t version = fd_io::ReadU32(header + 4);

  if (magic != kWalMagic) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kWalCorrupted, "Invalid WAL magic number", path));
  }
  if (version != kWalVersion) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalCorrupted, "Unsupported WAL version: " + std::to_string(version), path));
  }

  return {};
}

Expected<void, Error> WriteAheadLog::ScanExistingFiles() {
  files_.clear();
  current_sequence_ = 0;
  current_file_number_ = 0;

  DIR* dir = ::opendir(config_.directory.c_str());
  if (dir == nullptr) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kWalReadError,
                                                  "Failed to open WAL directory: " + std::string(std::strerror(errno)),
                                                  config_.directory));
  }

  std::vector<uint32_t> file_numbers;
  struct dirent* entry = nullptr;
  while ((entry = ::readdir(dir)) != nullptr) {
    std::string name(entry->d_name);
    // Match wal-NNNNNN.log
    if (name.size() == kFileNameLength && name.compare(0, kSegmentPrefix.size(), kSegmentPrefix) == 0 &&
        name.compare(kSegmentPrefix.size() + kFileNumberDigits, kSegmentSuffix.size(), kSegmentSuffix) == 0) {
      std::string num_str = name.substr(kSegmentPrefix.size(), kFileNumberDigits);
      if (!std::all_of(num_str.begin(), num_str.end(),
                       [](unsigned char character) { return std::isdigit(character) != 0; })) {
        continue;
      }
      try {
        auto num = static_cast<uint32_t>(std::stoul(num_str));
        file_numbers.push_back(num);
      } catch (...) {
        // Skip malformed filenames
      }
    }
  }
  ::closedir(dir);

  // Sort by file number
  std::sort(file_numbers.begin(), file_numbers.end());
  if (std::adjacent_find(file_numbers.begin(), file_numbers.end()) != file_numbers.end()) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kWalCorrupted, "Duplicate WAL segment number"));
  }

  // Scan each file to find sequence ranges
  std::optional<uint64_t> previous_sequence;
  for (size_t file_index = 0; file_index < file_numbers.size(); ++file_index) {
    const uint32_t num = file_numbers[file_index];
    const bool latest_segment = file_index + 1 == file_numbers.size();
    std::string path = MakeFilePath(num);
    struct stat initial_info {};
    auto opened = OpenWalSegment(path, latest_segment ? O_RDWR : O_RDONLY, initial_info);
    if (!opened) {
      return utils::MakeUnexpected(opened.error());
    }
    const int fd = *opened;

    WalFile wf;
    wf.path = path;
    wf.file_number = num;

    // A crash during rotation can leave the newest segment with a partial file
    // header. It holds no record, so it is reported empty and Open removes it.
    if (latest_segment && static_cast<uint64_t>(initial_info.st_size) < kWalFileHeaderSize) {
      ::close(fd);
      current_file_number_ = std::max(current_file_number_, num);
      files_.push_back(std::move(wf));
      continue;
    }

    auto header_result = ValidateFileHeader(fd, path);
    if (!header_result) {
      ::close(fd);
      return utils::MakeUnexpected(header_result.error());
    }
    wf.max_sequence = 0;
    off_t last_valid_offset = kWalFileHeaderSize;

    // Scan records to find the max sequence
    while (true) {
      uint8_t rec_header[8];
      const off_t record_offset = ::lseek(fd, 0, SEEK_CUR);
      if (record_offset < 0 || record_offset > initial_info.st_size) {
        ::close(fd);
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kWalReadError, "Invalid offset in WAL segment: " + path));
      }
      const uint64_t remaining = static_cast<uint64_t>(initial_info.st_size - record_offset);
      if (remaining == 0) {
        break;
      }
      if (remaining < sizeof(rec_header)) {
        auto repaired = RepairTornTail(latest_segment, fd, last_valid_offset, path, "Torn WAL header");
        if (!repaired) {
          ::close(fd);
          return utils::MakeUnexpected(repaired.error());
        }
        break;
      }
      if (!fd_io::ReadAll(fd, rec_header, sizeof(rec_header))) {
        ::close(fd);
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kWalReadError, "Failed to read WAL record header: " + path));
      }

      uint32_t body_length = fd_io::ReadU32(rec_header);
      uint32_t expected_crc = fd_io::ReadU32(rec_header + 4);
      if (body_length < kWalRecordHeaderSize || body_length > kMaxWalRecordBodySize) {
        ::close(fd);
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kWalCorrupted, "Invalid WAL record length: " + path));
      }
      if (remaining - sizeof(rec_header) < body_length) {
        auto repaired = RepairTornTail(latest_segment, fd, last_valid_offset, path, "Torn WAL body");
        if (!repaired) {
          ::close(fd);
          return utils::MakeUnexpected(repaired.error());
        }
        break;
      }

      std::vector<uint8_t> body(body_length);
      if (!fd_io::ReadAll(fd, body.data(), body_length)) {
        ::close(fd);
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kWalReadError, "Failed to read WAL record body: " + path));
      }
      if (snapshot_v1::CalculateCRC32(body.data(), body.size()) != expected_crc) {
        ::close(fd);
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kWalCRCMismatch, "WAL record CRC mismatch: " + path));
      }

      uint64_t seq = fd_io::ReadU64(body.data());
      if (seq == 0) {
        ::close(fd);
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kWalCorrupted, "WAL sequence zero is invalid in " + path));
      }
      if (previous_sequence.has_value() &&
          (*previous_sequence == std::numeric_limits<uint64_t>::max() || seq != *previous_sequence + 1)) {
        ::close(fd);
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kWalCorrupted,
                             "WAL sequence is not contiguous: previous=" + std::to_string(*previous_sequence) +
                                 ", current=" + std::to_string(seq)));
      }
      previous_sequence = seq;
      if (seq > wf.max_sequence)
        wf.max_sequence = seq;
      last_valid_offset = ::lseek(fd, 0, SEEK_CUR);
    }

    ::close(fd);

    if (wf.max_sequence > current_sequence_) {
      current_sequence_ = wf.max_sequence;
    }
    if (num > current_file_number_) {
      current_file_number_ = num;
    }

    files_.push_back(std::move(wf));
  }

  return {};
}

std::string WriteAheadLog::MakeFilePath(uint32_t file_number) const {
  char digits[32];
  std::snprintf(digits, sizeof(digits), "%0*u", kFileNumberDigits, file_number);
  return config_.directory + "/" + std::string(kSegmentPrefix) + digits + std::string(kSegmentSuffix);
}

void WriteAheadLog::SyncLoop() {
  while (sync_running_.load()) {
    {
      std::unique_lock<std::mutex> lock(sync_mutex_);
      sync_cv_.wait_for(lock, std::chrono::milliseconds(config_.sync_interval_ms),
                        [this] { return !sync_running_.load(); });
    }

    if (!sync_running_.load())
      break;

    if (needs_sync_.exchange(false)) {
      std::lock_guard<std::mutex> wal_lock(mutex_);
      if (current_fd_ >= 0) {
        if (::fsync(current_fd_) != 0) {
          const std::string message = "Asynchronous WAL fsync failed: " + std::string(std::strerror(errno));
          ::close(current_fd_);
          current_fd_ = -1;
          open_ = false;
          utils::StructuredLog().Event("wal_async_fsync_failed").Field("error", message).Error();
        }
      }
    }
  }
}

}  // namespace nvecd::storage

/**
 * @file wal.cpp
 * @brief Lightweight Write-Ahead Log implementation
 */

#include "storage/wal.h"

#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils/structured_log.h"

namespace nvecd::storage {

namespace {

/// Calculate CRC32 using zlib
uint32_t CalcCRC32(const void* data, size_t length) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(data), length));
}

/// Get current time in microseconds
uint64_t NowMicros() {
  auto now = std::chrono::system_clock::now();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          now.time_since_epoch())
          .count());
}

/// Write exactly n bytes to fd
bool WriteAll(int fd, const void* buf, size_t n) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto ptr = reinterpret_cast<const uint8_t*>(buf);
  size_t remaining = n;
  while (remaining > 0) {
    auto written = ::write(fd, ptr, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    ptr += written;
    remaining -= static_cast<size_t>(written);
  }
  return true;
}

/// Read exactly n bytes from fd
bool ReadAll(int fd, void* buf, size_t n) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto ptr = reinterpret_cast<uint8_t*>(buf);
  size_t remaining = n;
  while (remaining > 0) {
    auto bytes_read = ::read(fd, ptr, remaining);
    if (bytes_read < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (bytes_read == 0) return false;  // EOF
    ptr += bytes_read;
    remaining -= static_cast<size_t>(bytes_read);
  }
  return true;
}

/// Write a uint32_t in little-endian
void WriteU32(uint8_t* buf, uint32_t val) {
  std::memcpy(buf, &val, sizeof(val));
}

/// Write a uint64_t in little-endian
void WriteU64(uint8_t* buf, uint64_t val) {
  std::memcpy(buf, &val, sizeof(val));
}

/// Read a uint32_t in little-endian
uint32_t ReadU32(const uint8_t* buf) {
  uint32_t val = 0;
  std::memcpy(&val, buf, sizeof(val));
  return val;
}

/// Read a uint64_t in little-endian
uint64_t ReadU64(const uint8_t* buf) {
  uint64_t val = 0;
  std::memcpy(&val, buf, sizeof(val));
  return val;
}

}  // namespace

// ---------------------------------------------------------------------------
// WriteAheadLog
// ---------------------------------------------------------------------------

WriteAheadLog::~WriteAheadLog() { Close(); }

Expected<void, Error> WriteAheadLog::Open(const Config& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (open_) {
    Close();
  }

  config_ = config;

  // Ensure directory exists
  if (::mkdir(config_.directory.c_str(), 0755) != 0 && errno != EEXIST) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kWalWriteError,
        "Failed to create WAL directory: " + std::string(std::strerror(errno)),
        config_.directory));
  }

  // Scan existing files to recover state
  auto scan_result = ScanExistingFiles();
  if (!scan_result) {
    return utils::MakeUnexpected(scan_result.error());
  }

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
  // Stop sync thread
  if (sync_running_.load()) {
    sync_running_.store(false);
    sync_cv_.notify_all();
    if (sync_thread_.joinable()) {
      sync_thread_.join();
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (current_fd_ >= 0) {
    ::fsync(current_fd_);
    ::close(current_fd_);
    current_fd_ = -1;
  }
  open_ = false;
}

Expected<uint64_t, Error> WriteAheadLog::Append(WalOpType op,
                                                 const void* payload,
                                                 uint32_t payload_size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_ || current_fd_ < 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalNotOpen, "WAL is not open"));
  }

  // Check if rotation is needed
  uint32_t record_body_size = kWalRecordHeaderSize + payload_size;
  uint32_t total_record_size = sizeof(uint32_t) + sizeof(uint32_t) + record_body_size;
  // length(4) + crc32(4) + record_header(17) + payload

  if (current_file_size_ + total_record_size > config_.max_file_size &&
      current_file_size_ > kWalFileHeaderSize) {
    // Finalize current file info
    if (!files_.empty()) {
      files_.back().max_sequence = current_sequence_;
      files_.back().file_size = current_file_size_;
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
  WriteU64(body.data(), seq);
  WriteU64(body.data() + 8, timestamp);
  body[16] = static_cast<uint8_t>(op);
  if (payload_size > 0 && payload != nullptr) {
    std::memcpy(body.data() + kWalRecordHeaderSize, payload, payload_size);
  }

  // Calculate CRC32 of the body
  uint32_t crc = CalcCRC32(body.data(), body.size());

  // Write: [length:u32] [crc32:u32] [body...]
  uint8_t header[8];
  WriteU32(header, record_body_size);
  WriteU32(header + 4, crc);

  if (!WriteAll(current_fd_, header, sizeof(header))) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kWalWriteError,
        "Failed to write record header: " + std::string(std::strerror(errno))));
  }
  if (!WriteAll(current_fd_, body.data(), body.size())) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kWalWriteError,
        "Failed to write record body: " + std::string(std::strerror(errno))));
  }

  current_file_size_ += total_record_size;

  // Update file metadata
  if (!files_.empty()) {
    files_.back().max_sequence = seq;
    files_.back().file_size = current_file_size_;
  }

  if (config_.sync_on_write) {
    ::fsync(current_fd_);
  } else {
    needs_sync_.store(true);
  }

  return seq;
}

Expected<uint64_t, Error> WriteAheadLog::Replay(
    uint64_t from_sequence,
    const std::function<void(const WalRecord&)>& callback) const {
  std::lock_guard<std::mutex> lock(mutex_);

  uint64_t count = 0;

  // Collect relevant file paths (sorted by file number)
  std::vector<std::string> paths;
  for (const auto& f : files_) {
    // Include file if it might contain records >= from_sequence
    if (f.max_sequence >= from_sequence || f.max_sequence == 0) {
      paths.push_back(f.path);
    }
  }

  for (const auto& path : paths) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) continue;

    // Validate file header
    auto header_result = ValidateFileHeader(fd, path);
    if (!header_result) {
      ::close(fd);
      continue;
    }

    // Read records
    while (true) {
      // Read length + crc
      uint8_t rec_header[8];
      if (!ReadAll(fd, rec_header, sizeof(rec_header))) {
        break;  // EOF or incomplete header — done with this file
      }

      uint32_t body_length = ReadU32(rec_header);
      uint32_t expected_crc = ReadU32(rec_header + 4);

      if (body_length < kWalRecordHeaderSize) {
        break;  // Invalid record — stop
      }

      // Read body
      std::vector<uint8_t> body(body_length);
      if (!ReadAll(fd, body.data(), body_length)) {
        break;  // Incomplete record — skip
      }

      // Verify CRC
      uint32_t actual_crc = CalcCRC32(body.data(), body.size());
      if (actual_crc != expected_crc) {
        // CRC mismatch — skip this record, continue to next
        utils::StructuredLog()
            .Event("wal_crc_mismatch")
            .Field("file", path)
            .Field("expected_crc", static_cast<int64_t>(expected_crc))
            .Field("actual_crc", static_cast<int64_t>(actual_crc))
            .Warn();
        continue;
      }

      // Parse record
      WalRecord record;
      record.sequence = ReadU64(body.data());
      record.timestamp_us = ReadU64(body.data() + 8);
      record.op = static_cast<WalOpType>(body[16]);

      if (record.sequence < from_sequence) {
        continue;  // Skip records before requested sequence
      }

      uint32_t payload_size = body_length - kWalRecordHeaderSize;
      if (payload_size > 0) {
        record.payload.assign(body.begin() + kWalRecordHeaderSize, body.end());
      }

      callback(record);
      ++count;
    }

    ::close(fd);
  }

  return count;
}

Expected<void, Error> WriteAheadLog::Truncate(uint64_t up_to_sequence) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<WalFile> remaining;
  uint64_t deleted_count = 0;

  for (auto& f : files_) {
    // Delete files where ALL records are <= up_to_sequence
    // (i.e., max_sequence <= up_to_sequence AND it's not the current file)
    if (f.max_sequence <= up_to_sequence && f.max_sequence > 0 &&
        f.file_number != current_file_number_) {
      if (::unlink(f.path.c_str()) == 0) {
        ++deleted_count;
      }
    } else {
      remaining.push_back(std::move(f));
    }
  }

  files_ = std::move(remaining);

  if (deleted_count > 0) {
    utils::StructuredLog()
        .Event("wal_truncated")
        .Field("deleted_files", static_cast<int64_t>(deleted_count))
        .Field("up_to_sequence", static_cast<int64_t>(up_to_sequence))
        .Field("remaining_files", static_cast<int64_t>(files_.size()))
        .Info();
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
    ::fsync(current_fd_);
    ::close(current_fd_);
    current_fd_ = -1;
  }

  ++current_file_number_;
  std::string path = MakeFilePath(current_file_number_);

  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kWalRotationFailed,
        "Failed to create WAL file: " + std::string(std::strerror(errno)),
        path));
  }

  current_fd_ = fd;
  current_file_size_ = 0;

  auto result = WriteFileHeader();
  if (!result) {
    ::close(current_fd_);
    current_fd_ = -1;
    return utils::MakeUnexpected(result.error());
  }

  // Add file to the list
  WalFile wf;
  wf.path = path;
  wf.file_number = current_file_number_;
  wf.min_sequence = current_sequence_ + 1;
  wf.max_sequence = 0;
  wf.file_size = current_file_size_;
  files_.push_back(std::move(wf));

  return {};
}

Expected<void, Error> WriteAheadLog::WriteFileHeader() {
  uint8_t header[kWalFileHeaderSize];
  WriteU32(header, kWalMagic);
  WriteU32(header + 4, kWalVersion);

  if (!WriteAll(current_fd_, header, sizeof(header))) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kWalWriteError, "Failed to write WAL file header"));
  }

  current_file_size_ = kWalFileHeaderSize;
  return {};
}

Expected<void, Error> WriteAheadLog::ValidateFileHeader(
    int fd, const std::string& path) {
  uint8_t header[kWalFileHeaderSize];
  if (!ReadAll(fd, header, sizeof(header))) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kWalReadError, "Failed to read WAL file header",
        path));
  }

  uint32_t magic = ReadU32(header);
  uint32_t version = ReadU32(header + 4);

  if (magic != kWalMagic) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kWalCorrupted, "Invalid WAL magic number", path));
  }
  if (version != kWalVersion) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kWalCorrupted,
        "Unsupported WAL version: " + std::to_string(version), path));
  }

  return {};
}

Expected<void, Error> WriteAheadLog::ScanExistingFiles() {
  files_.clear();
  current_sequence_ = 0;
  current_file_number_ = 0;

  DIR* dir = ::opendir(config_.directory.c_str());
  if (dir == nullptr) {
    return {};  // Directory doesn't exist yet — fresh start
  }

  std::vector<uint32_t> file_numbers;
  struct dirent* entry = nullptr;
  while ((entry = ::readdir(dir)) != nullptr) {
    std::string name(entry->d_name);
    // Match wal-NNNNNN.log
    if (name.size() == 14 && name.substr(0, 4) == "wal-" &&
        name.substr(10, 4) == ".log") {
      std::string num_str = name.substr(4, 6);
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

  // Scan each file to find sequence ranges
  for (uint32_t num : file_numbers) {
    std::string path = MakeFilePath(num);
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) continue;

    auto header_result = ValidateFileHeader(fd, path);
    if (!header_result) {
      ::close(fd);
      continue;
    }

    WalFile wf;
    wf.path = path;
    wf.file_number = num;
    wf.min_sequence = UINT64_MAX;
    wf.max_sequence = 0;

    // Scan records to find min/max sequence
    while (true) {
      uint8_t rec_header[8];
      if (!ReadAll(fd, rec_header, sizeof(rec_header))) break;

      uint32_t body_length = ReadU32(rec_header);
      if (body_length < kWalRecordHeaderSize) break;

      // Read just enough for sequence number
      std::vector<uint8_t> body(body_length);
      if (!ReadAll(fd, body.data(), body_length)) break;

      uint64_t seq = ReadU64(body.data());
      if (seq < wf.min_sequence) wf.min_sequence = seq;
      if (seq > wf.max_sequence) wf.max_sequence = seq;
    }

    struct stat st {};
    if (::fstat(fd, &st) == 0) {
      wf.file_size = static_cast<uint64_t>(st.st_size);
    }
    ::close(fd);

    if (wf.min_sequence == UINT64_MAX) {
      wf.min_sequence = 0;  // Empty file
    }

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
  char buf[32];
  std::snprintf(buf, sizeof(buf), "wal-%06u.log", file_number);
  return config_.directory + "/" + buf;
}

void WriteAheadLog::SyncLoop() {
  while (sync_running_.load()) {
    std::unique_lock<std::mutex> lock(sync_mutex_);
    sync_cv_.wait_for(lock,
                      std::chrono::milliseconds(config_.sync_interval_ms));

    if (!sync_running_.load()) break;

    if (needs_sync_.exchange(false)) {
      std::lock_guard<std::mutex> wal_lock(mutex_);
      if (current_fd_ >= 0) {
        ::fsync(current_fd_);
      }
    }
  }
}

}  // namespace nvecd::storage

/**
 * @file wal_checkpoint.cpp
 * @brief Snapshot/WAL checkpoint sidecar implementation
 */

#include "storage/wal_checkpoint.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

#include "utils/path_utils.h"

namespace nvecd::storage {

namespace {

constexpr uint32_t kCheckpointMagic = 0x5043574E;  // "NWCP" little-endian
constexpr uint32_t kCheckpointVersion = 1;
constexpr size_t kCheckpointSize = 32;
constexpr size_t kFrameCrcOffset = 28;
constexpr uint64_t kMaxBoundSnapshotSize = 3ULL * 1024ULL * 1024ULL * 1024ULL;

/// File mode for newly created sidecar files (rw-------).
constexpr mode_t kSidecarMode = 0600;

std::string SidecarPath(const std::string& snapshot_path) {
  return snapshot_path + kWalCheckpointSuffix;
}

/// Write exactly n bytes to fd, retrying on EINTR.
bool WriteAll(int fd, const void* buf, size_t n) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* ptr = reinterpret_cast<const uint8_t*>(buf);
  size_t remaining = n;
  while (remaining > 0) {
    const auto written = ::write(fd, ptr, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    ptr += written;
    remaining -= static_cast<size_t>(written);
  }
  return true;
}

/// Read exactly n bytes from fd, retrying on EINTR.
bool ReadAll(int fd, void* buf, size_t n) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* ptr = reinterpret_cast<uint8_t*>(buf);
  size_t remaining = n;
  while (remaining > 0) {
    const auto bytes_read = ::read(fd, ptr, remaining);
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (bytes_read == 0) {
      return false;  // EOF before the full record
    }
    ptr += bytes_read;
    remaining -= static_cast<size_t>(bytes_read);
  }
  return true;
}

void WriteU32(uint8_t* output, uint32_t value) {
  for (size_t i = 0; i < sizeof(value); ++i) {
    output[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFU);
  }
}

void WriteU64(uint8_t* output, uint64_t value) {
  for (size_t i = 0; i < sizeof(value); ++i) {
    output[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFU);
  }
}

uint32_t ReadU32(const uint8_t* input) {
  uint32_t value = 0;
  for (size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<uint32_t>(input[i]) << (8 * i);
  }
  return value;
}

uint64_t ReadU64(const uint8_t* input) {
  uint64_t value = 0;
  for (size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<uint64_t>(input[i]) << (8 * i);
  }
  return value;
}

uint32_t CalculateCrc32(const void* data, size_t length) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(data), length));
}

struct SnapshotBinding {
  uint64_t size = 0;
  uint32_t crc32 = 0;
};

utils::Expected<SnapshotBinding, utils::Error> ReadSnapshotBinding(const std::string& snapshot_path) {
  const int fd = ::open(snapshot_path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalReadError,
                         "Failed to open checkpoint snapshot: " + std::string(std::strerror(errno)), snapshot_path));
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<uint64_t>(info.st_size) > kMaxBoundSnapshotSize) {
    const int saved_errno = errno;
    ::close(fd);
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kStorageCorrupted,
        "Checkpoint snapshot is not a bounded regular file: " + std::string(std::strerror(saved_errno)),
        snapshot_path));
  }

  std::array<uint8_t, 64 * 1024> buffer{};
  uint64_t remaining = static_cast<uint64_t>(info.st_size);
  uLong crc = crc32(0L, Z_NULL, 0);
  while (remaining > 0) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
    if (!ReadAll(fd, buffer.data(), chunk)) {
      const int saved_errno = errno;
      ::close(fd);
      return utils::MakeUnexpected(utils::MakeError(
          utils::ErrorCode::kWalReadError,
          "Failed to read checkpoint snapshot: " + std::string(std::strerror(saved_errno)), snapshot_path));
    }
    crc = crc32(crc, buffer.data(), static_cast<uInt>(chunk));
    remaining -= chunk;
  }
  if (::close(fd) != 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalReadError,
                         "Failed to close checkpoint snapshot: " + std::string(std::strerror(errno)), snapshot_path));
  }
  return SnapshotBinding{static_cast<uint64_t>(info.st_size), static_cast<uint32_t>(crc)};
}

/// Persist the directory entry created by rename(2). fsync'ing only the file
/// is not sufficient: after a power loss the directory can lose the rename and
/// leave a completed snapshot without its checkpoint sidecar.
bool FsyncParentDirectory(const std::string& path) {
  const auto parent = std::filesystem::path(path).parent_path();
  const std::string directory = parent.empty() ? "." : parent.string();
  const int dir_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
  if (dir_fd < 0) {
    return false;
  }
  const bool ok = ::fsync(dir_fd) == 0;
  ::close(dir_fd);
  return ok;
}

}  // namespace

utils::Expected<void, utils::Error> WriteWalCheckpoint(const std::string& snapshot_path, uint64_t sequence) {
  if (sequence == std::numeric_limits<uint64_t>::max()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kStorageWriteError, "Cannot checkpoint exhausted WAL sequence space"));
  }
  auto binding = ReadSnapshotBinding(snapshot_path);
  if (!binding) {
    return utils::MakeUnexpected(binding.error());
  }
  auto resolved_path_result = utils::ResolvePrivateStoragePath(SidecarPath(snapshot_path));
  if (!resolved_path_result) {
    return utils::MakeUnexpected(resolved_path_result.error());
  }
  const std::string final_path = resolved_path_result->string();

  std::string tmp_template = final_path + ".tmp.XXXXXX";
  std::vector<char> mutable_template(tmp_template.begin(), tmp_template.end());
  mutable_template.push_back('\0');

  std::array<uint8_t, kCheckpointSize> bytes{};
  WriteU32(bytes.data(), kCheckpointMagic);
  WriteU32(bytes.data() + 4, kCheckpointVersion);
  WriteU64(bytes.data() + 8, sequence);
  WriteU64(bytes.data() + 16, binding->size);
  WriteU32(bytes.data() + 24, binding->crc32);
  WriteU32(bytes.data() + kFrameCrcOffset, CalculateCrc32(bytes.data(), kFrameCrcOffset));

  const int fd = ::mkstemp(mutable_template.data());
  if (fd < 0) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kStorageWriteError,
                         "Failed to create WAL checkpoint temp file: " + std::string(std::strerror(errno))));
  }
  const std::string tmp_path(mutable_template.data());
  if (::fchmod(fd, kSidecarMode) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    ::unlink(tmp_path.c_str());
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kStorageWriteError,
                         "Failed to set WAL checkpoint permissions: " + std::string(std::strerror(saved_errno))));
  }

  if (!WriteAll(fd, bytes.data(), bytes.size())) {
    const int saved_errno = errno;
    ::close(fd);
    ::unlink(tmp_path.c_str());
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kStorageWriteError,
        "Failed to write WAL checkpoint '" + tmp_path + "': " + std::string(std::strerror(saved_errno))));
  }

  if (::fsync(fd) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    ::unlink(tmp_path.c_str());
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kStorageWriteError,
        "Failed to fsync WAL checkpoint '" + tmp_path + "': " + std::string(std::strerror(saved_errno))));
  }

  if (::close(fd) != 0) {
    const int saved_errno = errno;
    ::unlink(tmp_path.c_str());
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kStorageWriteError,
        "Failed to close WAL checkpoint '" + tmp_path + "': " + std::string(std::strerror(saved_errno))));
  }

  if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    const int saved_errno = errno;
    ::unlink(tmp_path.c_str());
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kStorageWriteError,
        "Failed to rename WAL checkpoint into place '" + final_path + "': " + std::string(std::strerror(saved_errno))));
  }

  if (!FsyncParentDirectory(final_path)) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kStorageWriteError,
        "Failed to fsync WAL checkpoint directory for '" + final_path + "': " + std::string(std::strerror(errno))));
  }

  return {};
}

utils::Expected<uint64_t, utils::Error> ReadWalCheckpoint(const std::string& snapshot_path) {
  auto binding = ReadSnapshotBinding(snapshot_path);
  if (!binding) {
    return utils::MakeUnexpected(binding.error());
  }
  const std::string final_path = SidecarPath(snapshot_path);

  const int fd = ::open(final_path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kWalReadError,
                                                  "Failed to open WAL checkpoint: " + std::string(std::strerror(errno)),
                                                  final_path));
  }

  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != static_cast<off_t>(kCheckpointSize)) {
    ::close(fd);
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kStorageCorrupted, "Invalid WAL checkpoint file type or size", final_path));
  }
  std::array<uint8_t, kCheckpointSize> bytes{};
  const bool ok = ReadAll(fd, bytes.data(), bytes.size());
  const bool closed = ::close(fd) == 0;
  if (!ok || !closed) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kWalReadError, "Failed to read WAL checkpoint frame", final_path));
  }

  if (ReadU32(bytes.data()) != kCheckpointMagic || ReadU32(bytes.data() + 4) != kCheckpointVersion) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kStorageCorrupted, "Invalid WAL checkpoint magic or version", final_path));
  }
  if (ReadU32(bytes.data() + kFrameCrcOffset) != CalculateCrc32(bytes.data(), kFrameCrcOffset)) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kStorageCorrupted, "WAL checkpoint CRC32 mismatch", final_path));
  }
  const uint64_t sequence = ReadU64(bytes.data() + 8);
  if (sequence == std::numeric_limits<uint64_t>::max()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kStorageCorrupted, "WAL checkpoint sequence is exhausted", final_path));
  }
  if (ReadU64(bytes.data() + 16) != binding->size || ReadU32(bytes.data() + 24) != binding->crc32) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kStorageCorrupted, "WAL checkpoint does not match snapshot", final_path));
  }
  return sequence;
}

}  // namespace nvecd::storage

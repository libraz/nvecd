/**
 * @file wal_checkpoint.cpp
 * @brief Snapshot/WAL checkpoint sidecar implementation
 */

#include "storage/wal_checkpoint.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <vector>

#include "utils/path_utils.h"

namespace nvecd::storage {

namespace {

/// Sidecar payload size: a single uint64 little-endian sequence number.
constexpr size_t kCheckpointSize = sizeof(uint64_t);

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
  auto resolved_path_result = utils::ResolvePrivateStoragePath(SidecarPath(snapshot_path));
  if (!resolved_path_result) {
    return utils::MakeUnexpected(resolved_path_result.error());
  }
  const std::string final_path = resolved_path_result->string();

  std::string tmp_template = final_path + ".tmp.XXXXXX";
  std::vector<char> mutable_template(tmp_template.begin(), tmp_template.end());
  mutable_template.push_back('\0');

  std::array<uint8_t, kCheckpointSize> bytes{};
  for (size_t i = 0; i < kCheckpointSize; ++i) {
    bytes[i] = static_cast<uint8_t>((sequence >> (8 * i)) & 0xFF);
  }

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

  ::close(fd);

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

uint64_t ReadWalCheckpoint(const std::string& snapshot_path) {
  const std::string final_path = SidecarPath(snapshot_path);

  const int fd = ::open(final_path.c_str(), O_RDONLY);
  if (fd < 0) {
    return 0;  // Sidecar absent: treat as "no checkpoint".
  }

  std::array<uint8_t, kCheckpointSize> bytes{};
  const bool ok = ReadAll(fd, bytes.data(), bytes.size());
  ::close(fd);
  if (!ok) {
    return 0;  // Truncated or unreadable: treat as "no checkpoint".
  }

  uint64_t sequence = 0;
  for (size_t i = 0; i < kCheckpointSize; ++i) {
    sequence |= static_cast<uint64_t>(bytes[i]) << (8 * i);
  }
  return sequence;
}

}  // namespace nvecd::storage

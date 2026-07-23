/**
 * @file path_utils.h
 * @brief Path validation utilities for dump file operations
 */

#pragma once

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::utils {

/** Reject replaceable non-sticky ancestors on the way to a storage directory. */
inline Expected<void, Error> ValidateSecureDirectoryAncestors(const std::filesystem::path& directory) {
#ifndef _WIN32
  std::error_code error;
  const auto canonical = std::filesystem::canonical(directory, error);
  if (error) {
    return MakeUnexpected(
        MakeError(ErrorCode::kPermissionDenied, "Cannot resolve directory ancestors: " + directory.string()));
  }
  std::filesystem::path current = canonical.root_path();
  for (const auto& component : canonical.relative_path()) {
    current /= component;
    struct stat info {};
    if (::lstat(current.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
      return MakeUnexpected(MakeError(ErrorCode::kPermissionDenied, "Invalid directory ancestor: " + current.string()));
    }
    const bool shared_writable = (info.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    const bool sticky = (info.st_mode & S_ISVTX) != 0;
    if (shared_writable && !sticky) {
      return MakeUnexpected(MakeError(ErrorCode::kPermissionDenied,
                                      "Directory ancestor is replaceable by another user: " + current.string()));
    }
  }
#else
  (void)directory;
#endif
  return {};
}

/**
 * @brief Require a directory owned by the service and not writable by others.
 *
 * Persistent snapshots and WALs are written with privileged service access.
 * Rejecting a shared writable directory prevents path replacement attacks
 * between path validation and the atomic rename step.
 */
inline Expected<void, Error> ValidatePrivateDirectory(const std::filesystem::path& directory) {
#ifndef _WIN32
  auto ancestors_valid = ValidateSecureDirectoryAncestors(directory);
  if (!ancestors_valid) {
    return ancestors_valid;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open requires mode only with O_CREAT.
  const int directory_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (directory_fd < 0) {
    return MakeUnexpected(
        MakeError(ErrorCode::kPermissionDenied, "Cannot securely open storage directory: " + directory.string()));
  }
  struct stat info {};
  if (::fstat(directory_fd, &info) != 0) {
    ::close(directory_fd);
    return MakeUnexpected(
        MakeError(ErrorCode::kPermissionDenied, "Cannot inspect storage directory: " + directory.string()));
  }
  ::close(directory_fd);
  if (!S_ISDIR(info.st_mode)) {
    return MakeUnexpected(
        MakeError(ErrorCode::kInvalidArgument, "Storage path is not a directory: " + directory.string()));
  }
  if (info.st_uid != ::geteuid()) {
    return MakeUnexpected(MakeError(ErrorCode::kPermissionDenied,
                                    "Storage directory must be owned by the service user: " + directory.string()));
  }
  if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return MakeUnexpected(
        MakeError(ErrorCode::kPermissionDenied,
                  "Storage directory must not be writable by group or others: " + directory.string()));
  }
#else
  (void)directory;
#endif
  return {};
}

/**
 * @brief Resolve a storage filepath through a private canonical parent directory.
 *
 * The returned path no longer contains a parent-directory symlink. Callers
 * must use this returned path for both temporary-file creation and rename so
 * that validation cannot be invalidated by replacing an input-path symlink.
 */
inline Expected<std::filesystem::path, Error> ResolvePrivateStoragePath(const std::filesystem::path& filepath) {
  const auto parent = filepath.parent_path().empty() ? std::filesystem::path(".") : filepath.parent_path();
  std::error_code error;
  const auto canonical_parent = std::filesystem::canonical(parent, error);
  if (error) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument,
                                    "Invalid storage directory: " + parent.string() + ": " + error.message()));
  }
  auto directory_valid = ValidatePrivateDirectory(canonical_parent);
  if (!directory_valid) {
    return MakeUnexpected(directory_valid.error());
  }
  if (filepath.filename().empty() || filepath.filename() == "." || filepath.filename() == "..") {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Storage path must name a file"));
  }
  return canonical_parent / filepath.filename();
}

#ifndef _WIN32

/** A close-on-destruction descriptor for a securely opened regular file. */
class SecureFileDescriptor {
 public:
  SecureFileDescriptor() = default;
  explicit SecureFileDescriptor(int fd) : fd_(fd) {}
  ~SecureFileDescriptor() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  SecureFileDescriptor(const SecureFileDescriptor&) = delete;
  SecureFileDescriptor& operator=(const SecureFileDescriptor&) = delete;
  SecureFileDescriptor(SecureFileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  SecureFileDescriptor& operator=(SecureFileDescriptor&& other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) {
        ::close(fd_);
      }
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  [[nodiscard]] int Get() const { return fd_; }
  [[nodiscard]] std::string StreamPath() const { return "/dev/fd/" + std::to_string(fd_); }

 private:
  int fd_ = -1;
};

/** A private temporary file whose directory entry is removed unless published. */
class SecureTemporaryFile {
 public:
  SecureTemporaryFile() = default;
  SecureTemporaryFile(int fd, int directory_fd, std::string filename)
      : fd_(fd), directory_fd_(directory_fd), filename_(std::move(filename)) {}
  ~SecureTemporaryFile() {
    if (!published_ && directory_fd_ >= 0 && !filename_.empty()) {
      ::unlinkat(directory_fd_, filename_.c_str(), 0);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
    if (directory_fd_ >= 0) {
      ::close(directory_fd_);
    }
  }

  SecureTemporaryFile(const SecureTemporaryFile&) = delete;
  SecureTemporaryFile& operator=(const SecureTemporaryFile&) = delete;
  SecureTemporaryFile(SecureTemporaryFile&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)),
        directory_fd_(std::exchange(other.directory_fd_, -1)),
        filename_(std::move(other.filename_)),
        published_(std::exchange(other.published_, true)) {}
  SecureTemporaryFile& operator=(SecureTemporaryFile&& other) noexcept {
    if (this != &other) {
      if (!published_ && directory_fd_ >= 0 && !filename_.empty()) {
        ::unlinkat(directory_fd_, filename_.c_str(), 0);
      }
      if (fd_ >= 0) {
        ::close(fd_);
      }
      if (directory_fd_ >= 0) {
        ::close(directory_fd_);
      }
      fd_ = std::exchange(other.fd_, -1);
      directory_fd_ = std::exchange(other.directory_fd_, -1);
      filename_ = std::move(other.filename_);
      published_ = std::exchange(other.published_, true);
    }
    return *this;
  }

  [[nodiscard]] int Get() const { return fd_; }
  [[nodiscard]] const std::string& Filename() const { return filename_; }
  [[nodiscard]] std::string StreamPath() const { return "/dev/fd/" + std::to_string(fd_); }
  void MarkPublished() { published_ = true; }

 private:
  int fd_ = -1;
  int directory_fd_ = -1;
  std::string filename_;
  bool published_ = false;
};

/**
 * An opened private parent directory plus a single leaf name.
 *
 * All file operations remain relative to directory_fd_, so renaming or
 * replacing an ancestor after validation cannot redirect an operation.
 */
class PrivateStorageTarget {
 public:
  PrivateStorageTarget() = default;
  ~PrivateStorageTarget() {
    if (directory_fd_ >= 0) {
      ::close(directory_fd_);
    }
  }

  PrivateStorageTarget(const PrivateStorageTarget&) = delete;
  PrivateStorageTarget& operator=(const PrivateStorageTarget&) = delete;
  PrivateStorageTarget(PrivateStorageTarget&& other) noexcept
      : directory_fd_(std::exchange(other.directory_fd_, -1)),
        filename_(std::move(other.filename_)),
        display_path_(std::move(other.display_path_)) {}
  PrivateStorageTarget& operator=(PrivateStorageTarget&& other) noexcept {
    if (this != &other) {
      if (directory_fd_ >= 0) {
        ::close(directory_fd_);
      }
      directory_fd_ = std::exchange(other.directory_fd_, -1);
      filename_ = std::move(other.filename_);
      display_path_ = std::move(other.display_path_);
    }
    return *this;
  }

  static Expected<PrivateStorageTarget, Error> Open(const std::filesystem::path& filepath) {
    auto resolved = ResolvePrivateStoragePath(filepath);
    if (!resolved) {
      return MakeUnexpected(resolved.error());
    }
    const auto parent = resolved->parent_path();
    const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
      return MakeUnexpected(MakeError(ErrorCode::kPermissionDenied,
                                      "Cannot securely open storage directory: " + std::string(std::strerror(errno))));
    }

    struct stat opened {};
    struct stat named {};
    if (::fstat(fd, &opened) != 0 || ::lstat(parent.c_str(), &named) != 0 || !S_ISDIR(opened.st_mode) ||
        opened.st_uid != ::geteuid() || (opened.st_mode & (S_IWGRP | S_IWOTH)) != 0 || opened.st_dev != named.st_dev ||
        opened.st_ino != named.st_ino) {
      ::close(fd);
      return MakeUnexpected(
          MakeError(ErrorCode::kPermissionDenied, "Storage directory changed during secure open: " + parent.string()));
    }
    return PrivateStorageTarget(fd, resolved->filename().string(), resolved->string());
  }

  Expected<SecureFileDescriptor, Error> OpenRegularFileReadOnly() const {
    const int fd = ::openat(directory_fd_, filename_.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpReadError,
                                      "Failed to securely open storage file: " + std::string(std::strerror(errno))));
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      ::close(fd);
      return MakeUnexpected(
          MakeError(ErrorCode::kPermissionDenied, "Storage file is not a private service-owned regular file"));
    }
    return SecureFileDescriptor(fd);
  }

  Expected<SecureTemporaryFile, Error> CreateTemporaryFile() const {
    static std::atomic<uint64_t> counter{0};
    for (size_t attempt = 0; attempt < 128; ++attempt) {
      const std::string temporary_name = "." + filename_ + ".tmp." + std::to_string(::getpid()) + "." +
                                         std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
      const int fd = ::openat(directory_fd_, temporary_name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC,
                              S_IRUSR | S_IWUSR);
      if (fd < 0) {
        if (errno == EEXIST) {
          continue;
        }
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpWriteError,
                      "Failed to securely create snapshot temporary file: " + std::string(std::strerror(errno))));
      }
      struct stat info {};
      if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
          (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        ::close(fd);
        ::unlinkat(directory_fd_, temporary_name.c_str(), 0);
        return MakeUnexpected(
            MakeError(ErrorCode::kPermissionDenied, "Snapshot temporary file failed opened-inode validation"));
      }
      const int cleanup_directory_fd = ::dup(directory_fd_);
      if (cleanup_directory_fd < 0) {
        const int saved_errno = errno;
        ::close(fd);
        ::unlinkat(directory_fd_, temporary_name.c_str(), 0);
        return MakeUnexpected(
            MakeError(ErrorCode::kStorageDumpWriteError,
                      "Failed to retain snapshot directory descriptor: " + std::string(std::strerror(saved_errno))));
      }
      return SecureTemporaryFile(fd, cleanup_directory_fd, temporary_name);
    }
    return MakeUnexpected(
        MakeError(ErrorCode::kStorageDumpWriteError, "Could not allocate a unique snapshot temporary filename"));
  }

  Expected<void, Error> Publish(SecureTemporaryFile& temporary_file) const {
    struct stat opened {};
    struct stat named {};
    if (::fstat(temporary_file.Get(), &opened) != 0 ||
        ::fstatat(directory_fd_, temporary_file.Filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(named.st_mode) || opened.st_dev != named.st_dev || opened.st_ino != named.st_ino) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpWriteError, "Snapshot temporary file changed before publish"));
    }
    if (::renameat(directory_fd_, temporary_file.Filename().c_str(), directory_fd_, filename_.c_str()) != 0) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError,
                                      "Failed to publish snapshot: " + std::string(std::strerror(errno))));
    }
    temporary_file.MarkPublished();
    if (::fstatat(directory_fd_, filename_.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(named.st_mode) ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino) {
      return MakeUnexpected(
          MakeError(ErrorCode::kStorageDumpWriteError, "Published snapshot failed opened-inode validation"));
    }
    return {};
  }

  Expected<void, Error> FsyncDirectory() const {
    if (::fsync(directory_fd_) != 0) {
      return MakeUnexpected(MakeError(ErrorCode::kStorageDumpWriteError,
                                      "Failed to fsync snapshot directory: " + std::string(std::strerror(errno))));
    }
    return {};
  }

  [[nodiscard]] const std::string& DisplayPath() const { return display_path_; }

 private:
  PrivateStorageTarget(int directory_fd, std::string filename, std::string display_path)
      : directory_fd_(directory_fd), filename_(std::move(filename)), display_path_(std::move(display_path)) {}

  int directory_fd_ = -1;
  std::string filename_;
  std::string display_path_;
};

#endif

/**
 * @brief Validate and canonicalize a dump file path
 *
 * If the filepath is not absolute, it is prepended with dump_dir.
 * The resulting path is canonicalized and checked for path traversal
 * (i.e., the canonical path must reside within the dump directory).
 *
 * @param filepath The raw file path (absolute or relative)
 * @param dump_dir The allowed dump directory
 * @return The validated canonical path, or an error on failure
 */
inline Expected<std::string, Error> ValidateDumpPath(const std::string& filepath, const std::string& dump_dir) {
  // Defense-in-depth: reject paths containing ".." segments before canonicalization
  if (filepath.find("..") != std::string::npos) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Invalid filepath: path traversal detected"));
  }

  std::string resolved = filepath;
  if (!resolved.empty() && resolved[0] != '/') {
    resolved = dump_dir + "/" + resolved;
  }

  // Use non-throwing overloads to comply with Expected<T,Error> pattern
  std::error_code ec;

  // Use canonical() for dump_dir (must exist) to resolve symlinks
  std::filesystem::path dump_canonical = std::filesystem::canonical(dump_dir, ec);
  if (ec) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Invalid dump directory: " + ec.message()));
  }
  auto directory_valid = ValidatePrivateDirectory(dump_canonical);
  if (!directory_valid) {
    return MakeUnexpected(directory_valid.error());
  }

  // Use weakly_canonical for the target path (may not exist yet)
  std::filesystem::path resolved_canonical = std::filesystem::weakly_canonical(resolved, ec);
  if (ec) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Invalid filepath: " + ec.message()));
  }

  // Verify resolved path resides within dump directory
  auto rel = resolved_canonical.lexically_relative(dump_canonical);
  if (rel.empty() || rel.string().substr(0, 2) == "..") {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Invalid filepath: path traversal detected"));
  }

  const auto target_parent =
      resolved_canonical.parent_path().empty() ? dump_canonical : resolved_canonical.parent_path();
  auto target_directory_valid = ValidatePrivateDirectory(target_parent);
  if (!target_directory_valid) {
    return MakeUnexpected(target_directory_valid.error());
  }

  return resolved_canonical.string();
}

}  // namespace nvecd::utils

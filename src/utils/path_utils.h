/**
 * @file path_utils.h
 * @brief Path validation utilities for dump file operations
 */

#pragma once

#include <filesystem>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::utils {

/**
 * @brief Require a directory owned by the service and not writable by others.
 *
 * Persistent snapshots and WALs are written with privileged service access.
 * Rejecting a shared writable directory prevents path replacement attacks
 * between path validation and the atomic rename step.
 */
inline Expected<void, Error> ValidatePrivateDirectory(const std::filesystem::path& directory) {
#ifndef _WIN32
  struct stat info {};
  if (::stat(directory.c_str(), &info) != 0) {
    return MakeUnexpected(
        MakeError(ErrorCode::kPermissionDenied, "Cannot inspect storage directory: " + directory.string()));
  }
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
  return canonical_parent / filepath.filename();
}

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

  return resolved_canonical.string();
}

}  // namespace nvecd::utils

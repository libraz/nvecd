/**
 * @file path_utils.h
 * @brief Path validation utilities for dump file operations
 */

#pragma once

#include <filesystem>
#include <string>

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::utils {

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

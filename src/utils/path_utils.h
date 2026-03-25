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
  std::string resolved = filepath;
  if (!resolved.empty() && resolved[0] != '/') {
    resolved = dump_dir + "/" + resolved;
  }

  try {
    std::filesystem::path canonical = std::filesystem::weakly_canonical(resolved);
    std::filesystem::path dump_canonical = std::filesystem::weakly_canonical(dump_dir);
    auto rel = canonical.lexically_relative(dump_canonical);
    if (rel.empty() || rel.string().substr(0, 2) == "..") {
      return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, "Invalid filepath: path traversal detected"));
    }
    return canonical.string();
  } catch (const std::exception& e) {
    return MakeUnexpected(MakeError(ErrorCode::kInvalidArgument, std::string("Invalid filepath: ") + e.what()));
  }
}

}  // namespace nvecd::utils

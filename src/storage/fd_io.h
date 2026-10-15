/**
 * @file fd_io.h
 * @brief Internal file-descriptor and byte-order helpers shared by the WAL and snapshot sidecars
 *
 * On-disk integers written through these helpers are little-endian on every host.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nvecd::storage::fd_io {

/// Write exactly n bytes to fd, retrying on EINTR.
bool WriteAll(int fd, const void* buf, size_t n);

/// Read exactly n bytes from fd, retrying on EINTR. EOF before n bytes is a failure.
bool ReadAll(int fd, void* buf, size_t n);

/// fsync a directory so entries created, renamed or removed in it are durable.
/// An empty path means the current directory. errno describes the failure.
bool FsyncDirectory(const std::string& directory);

inline void WriteU32(uint8_t* output, uint32_t value) {
  for (size_t i = 0; i < sizeof(value); ++i) {
    output[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFU);
  }
}

inline void WriteU64(uint8_t* output, uint64_t value) {
  for (size_t i = 0; i < sizeof(value); ++i) {
    output[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFU);
  }
}

inline uint32_t ReadU32(const uint8_t* input) {
  uint32_t value = 0;
  for (size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<uint32_t>(input[i]) << (8 * i);
  }
  return value;
}

inline uint64_t ReadU64(const uint8_t* input) {
  uint64_t value = 0;
  for (size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<uint64_t>(input[i]) << (8 * i);
  }
  return value;
}

}  // namespace nvecd::storage::fd_io

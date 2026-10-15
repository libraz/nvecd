/**
 * @file fd_io.cpp
 * @brief Internal file-descriptor helpers
 */

#include "storage/fd_io.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

namespace nvecd::storage::fd_io {

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
      return false;  // EOF
    }
    ptr += bytes_read;
    remaining -= static_cast<size_t>(bytes_read);
  }
  return true;
}

bool FsyncDirectory(const std::string& directory) {
  const int fd = ::open(directory.empty() ? "." : directory.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return false;
  }
  const bool synced = ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  return synced && closed;
}

}  // namespace nvecd::storage::fd_io

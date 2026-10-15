/**
 * @file io_config.h
 * @brief Connection I/O limits and the request callback shared by the reactor
 */

#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace nvecd::server {

struct ConnectionContext;

inline constexpr size_t kDefaultIORecvBufferSize = 4096;       // Separate from server_types.h to avoid conflicts
inline constexpr size_t kDefaultMaxQueryLength = 1024 * 1024;  // 1MB
inline constexpr size_t kDefaultMaxAccumulatedBytes = 2 * 1024 * 1024;  // 2MB
inline constexpr int kDefaultRecvTimeoutSec = 60;

/**
 * @brief Configuration for connection I/O handling
 */
struct IOConfig {
  size_t recv_buffer_size = kDefaultIORecvBufferSize;
  size_t max_query_length = kDefaultMaxQueryLength;
  size_t max_accumulated_bytes = kDefaultMaxAccumulatedBytes;
  int recv_timeout_sec = kDefaultRecvTimeoutSec;
};

/**
 * @brief Callback for processing complete requests
 * @param request The request string (without \r\n)
 * @param ctx Connection context (may be modified)
 * @return Response string (without \r\n)
 */
using RequestProcessor = std::function<std::string(const std::string&, ConnectionContext&)>;

}  // namespace nvecd::server

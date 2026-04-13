/**
 * @file server_types.h
 * @brief Common server type definitions for nvecd
 *
 * Reference: ../mygram-db/src/server/server_types.h
 * Reusability: 60% (removed MySQL/Index/DocumentStore dependencies)
 */

#pragma once

#include <atomic>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "config/config.h"
#include "utils/network_utils.h"

// Forward declarations (must be outside nvecd::server namespace)
namespace nvecd {
namespace events {
class EventStore;
class CoOccurrenceIndex;
}  // namespace events

namespace vectors {
class VectorStore;
class MetadataStore;
}  // namespace vectors

namespace similarity {
class SimilarityEngine;
}  // namespace similarity

namespace cache {
class SimilarityCache;
}  // namespace cache

namespace config {
class RuntimeVariableManager;
}  // namespace config

namespace storage {
class ForkSnapshotWriter;
}  // namespace storage
}  // namespace nvecd

namespace nvecd::server {

// Default constants
constexpr uint16_t kDefaultPort = 11017;       // nvecd default port
constexpr int kDefaultMaxConnections = 10000;  // Maximum concurrent connections
constexpr int kDefaultRecvBufferSize = 4096;   // Receive buffer size
constexpr int kDefaultSendBufferSize = 65536;  // Send buffer size

/**
 * @brief TCP server configuration
 */
struct ServerConfig {
  std::string host = "127.0.0.1";
  uint16_t port = kDefaultPort;
  int max_connections = kDefaultMaxConnections;
  int max_connections_per_ip = 100;  ///< Maximum connections per IP (0 = unlimited)
  int worker_threads = 0;  // Number of worker threads (0 = CPU count)
  int recv_buffer_size = kDefaultRecvBufferSize;
  int send_buffer_size = kDefaultSendBufferSize;
  std::vector<std::string> allow_cidrs;
  std::vector<utils::CIDR> parsed_allow_cidrs;
  std::string unix_socket_path;  ///< Unix socket path (empty = TCP mode)
};

/**
 * @brief Per-connection context
 */
struct ConnectionContext {
  int client_fd = -1;
  bool debug_mode = false;      ///< Debug mode flag
  bool authenticated = false;   ///< Whether client has authenticated
  std::string client_ip;        ///< Client IP address (for rate limiting and logging)
};

/**
 * @brief Thread-safe server statistics tracker
 *
 * Reference: ../mygram-db/src/server/server_stats.h
 * Reusability: 90% (adapted for nvecd command types)
 *
 * Uses std::atomic for thread-safe counter updates without locks.
 */
struct ServerStats {
  // Read-mostly (set once at startup)
  uint64_t start_time = static_cast<uint64_t>(std::time(nullptr));

  // Hot counters - separate cache lines for frequently updated counters
  alignas(64) std::atomic<uint64_t> total_connections{0};
  alignas(64) std::atomic<uint64_t> active_connections{0};
  alignas(64) std::atomic<uint64_t> total_commands{0};
  std::atomic<uint64_t> failed_commands{0};

  // Per-command type counters (less contended, grouped together)
  alignas(64) std::atomic<uint64_t> event_commands{0};
  std::atomic<uint64_t> sim_commands{0};
  std::atomic<uint64_t> vecset_commands{0};
  std::atomic<uint64_t> info_commands{0};
  std::atomic<uint64_t> config_commands{0};
  std::atomic<uint64_t> dump_commands{0};
  std::atomic<uint64_t> cache_commands{0};

  /**
   * @brief Get uptime in seconds
   * Reference: ../mygram-db/src/server/server_stats.cpp:GetUptimeSeconds
   */
  uint64_t GetUptimeSeconds() const { return static_cast<uint64_t>(std::time(nullptr)) - start_time; }

  /**
   * @brief Get queries per second
   */
  double GetQueriesPerSecond() const {
    uint64_t uptime = GetUptimeSeconds();
    if (uptime == 0) {
      return 0.0;
    }
    return static_cast<double>(total_commands.load()) / static_cast<double>(uptime);
  }
};

/**
 * @brief Server context for admin commands (INFO, CONFIG SHOW)
 */
struct ServerContext {
  const config::Config* config = nullptr;
  uint64_t uptime_seconds = 0;
  uint64_t connections_total = 0;
  uint64_t connections_current = 0;
  uint64_t vectors_total = 0;
  uint32_t vector_dimension = 0;
  uint64_t contexts_total = 0;
  uint64_t events_total = 0;
  bool cache_enabled = false;
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  uint64_t queries_total = 0;
  double queries_per_second = 0.0;
};

/**
 * @brief Context passed to command handlers
 *
 * Contains all necessary dependencies and state for command execution.
 * Reference members are intentional: this struct does not own the data,
 * it provides access to objects managed by TCPServer.
 */
struct HandlerContext {
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) - Intentional design: context references external
  // state

  // Core data stores
  events::EventStore* event_store = nullptr;
  events::CoOccurrenceIndex* co_index = nullptr;
  vectors::VectorStore* vector_store = nullptr;
  vectors::MetadataStore* metadata_store = nullptr;
  similarity::SimilarityEngine* similarity_engine = nullptr;
  std::atomic<cache::SimilarityCache*> cache{nullptr};

  // Runtime configuration
  config::RuntimeVariableManager* variable_manager = nullptr;

  ServerStats& stats;
  const config::Config* config = nullptr;
  std::atomic<bool>& loading;
  std::atomic<bool>& read_only;

  // Snapshot directory
  std::string dump_dir;

  // Security
  std::string requirepass;  ///< Required password (empty = no auth)

  // Snapshot fork writer (non-owning, owned by NvecdServer)
  storage::ForkSnapshotWriter* fork_snapshot_writer = nullptr;

  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

}  // namespace nvecd::server

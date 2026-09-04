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
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "config/config.h"
#include "server/command_types.h"
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
class SimilarityCacheController;
}  // namespace cache

namespace config {
class RuntimeVariableManager;
}  // namespace config

namespace storage {
class ForkSnapshotWriter;
class WriteAheadLog;
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
  int worker_threads = 0;            // Number of worker threads (0 = CPU count)
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
  bool debug_mode = false;     ///< Debug mode flag
  bool authenticated = false;  ///< Whether client has authenticated
  std::string client_ip;       ///< Client IP address (for rate limiting and logging)
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

  /// WAL records that recovery classified as an intended gap and skipped.
  /// Reported by INFO so the size of a recovery gap is observable rather than
  /// only visible as individual log lines.
  std::atomic<uint64_t> wal_replay_records_skipped{0};

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
 * @brief RAII accounting for one dispatched command
 *
 * Construction counts the command in ServerStats::total_commands and in the
 * per-type counter for @p type; destruction counts it in
 * ServerStats::failed_commands unless MarkSucceeded() has been called. Handler
 * bodies therefore never touch the counters, and adding an early return cannot
 * drop an increment or leave failed_commands above total_commands: every exit
 * path of a scope that owns a guard is accounted exactly once.
 *
 * The guard is created after a request has been recognised as a command, so
 * both surfaces count the same population: the TCP dispatcher creates one per
 * Dispatch() call (including a parse failure, as kUnknown), and the HTTP route
 * wrapper creates one per request to a route that mirrors a TCP command.
 */
class CommandStatsScope {
 public:
  CommandStatsScope(ServerStats& stats, CommandType type) : stats_(stats) {
    stats_.total_commands.fetch_add(1, std::memory_order_relaxed);
    CountByType(stats_, type);
  }

  CommandStatsScope(const CommandStatsScope&) = delete;
  CommandStatsScope& operator=(const CommandStatsScope&) = delete;
  CommandStatsScope(CommandStatsScope&&) = delete;
  CommandStatsScope& operator=(CommandStatsScope&&) = delete;

  ~CommandStatsScope() {
    if (!succeeded_) {
      stats_.failed_commands.fetch_add(1, std::memory_order_relaxed);
    }
  }

  /// Record that the client receives a success response for this command.
  void MarkSucceeded() { succeeded_ = true; }

 private:
  /// Per-type accounting. Types without a dedicated counter are listed
  /// explicitly so that adding a command type forces a decision here.
  static void CountByType(ServerStats& stats, CommandType type) {
    switch (type) {
      case CommandType::kEvent:
        stats.event_commands.fetch_add(1, std::memory_order_relaxed);
        return;
      case CommandType::kVecset:
        stats.vecset_commands.fetch_add(1, std::memory_order_relaxed);
        return;
      case CommandType::kSim:
      case CommandType::kSimv:
        stats.sim_commands.fetch_add(1, std::memory_order_relaxed);
        return;
      case CommandType::kInfo:
        stats.info_commands.fetch_add(1, std::memory_order_relaxed);
        return;
      case CommandType::kConfigHelp:
      case CommandType::kConfigShow:
      case CommandType::kConfigVerify:
      case CommandType::kSet:
      case CommandType::kGet:
      case CommandType::kShowVariables:
        stats.config_commands.fetch_add(1, std::memory_order_relaxed);
        return;
      case CommandType::kDumpSave:
      case CommandType::kDumpLoad:
      case CommandType::kDumpVerify:
      case CommandType::kDumpInfo:
      case CommandType::kDumpStatus:
        stats.dump_commands.fetch_add(1, std::memory_order_relaxed);
        return;
      case CommandType::kCacheStats:
      case CommandType::kCacheClear:
      case CommandType::kCacheEnable:
      case CommandType::kCacheDisable:
        stats.cache_commands.fetch_add(1, std::memory_order_relaxed);
        return;
      // Counted in total_commands only: these have no per-type counter.
      case CommandType::kVecdel:
      case CommandType::kMetaset:
      case CommandType::kDebugOn:
      case CommandType::kDebugOff:
      case CommandType::kAuth:
      case CommandType::kUnknown:
      case CommandType::kCount:
        return;
    }
  }

  ServerStats& stats_;
  bool succeeded_ = false;
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

  /// Directory a client-supplied CONFIG VERIFY path must resolve inside.
  ///
  /// Holds the directory of the configuration file the server was started
  /// with. CONFIG VERIFY opens files on behalf of a network client, so the
  /// path is canonicalised and confined to this root exactly as the sibling
  /// DUMP commands are confined to dump_dir. When the server was started
  /// without a configuration file this is empty and dump_dir is used as the
  /// root, so the command is never unrooted.
  std::string config_dir;

  // Snapshot fork writer (non-owning, owned by NvecdServer)
  storage::ForkSnapshotWriter* fork_snapshot_writer = nullptr;

  /// Write-Ahead Log for durability (non-owning, owned by NvecdServer).
  ///
  /// When non-null, write handlers append a record to the WAL before applying
  /// the in-memory mutation, so a record can never be missing for a change that
  /// is already visible. The pointer stays null during startup WAL replay so
  /// re-applied records are not logged a second time; the server only publishes
  /// it once replay completes.
  storage::WriteAheadLog* wal = nullptr;

  /// Vector-store generation counter.
  ///
  /// Bumped on every VECSET so that SIM/SIMV cache keys derived from the vector
  /// store change whenever a vector is added or updated. This invalidates
  /// vector-derived cached results even for brand-new item IDs, which the
  /// per-item reverse index cannot catch (a new ID is referenced by no existing
  /// cache entry). Shared by the TCP dispatcher and the HTTP server so both
  /// surfaces observe the same key space.
  std::atomic<uint64_t> vector_generation{0};

  /// Metadata generation used by filtered SIM/SIMV cache identities.
  std::atomic<uint64_t> metadata_generation{0};

  /// Whole-dataset publication generation, advanced by transactional LOAD.
  std::atomic<uint64_t> dataset_generation{0};

  /// Optional server-wide barrier for lock-mode snapshots. Core write handlers
  /// hold this shared for their full mutation/WAL sequence; a snapshot holds it
  /// exclusively after publishing read_only, so no write can cross the
  /// snapshot's point-in-time boundary.
  std::shared_mutex* snapshot_write_gate = nullptr;

  /// Serializes a core mutation with its WAL append. This preserves WAL order
  /// across VECSET/METASET from concurrent TCP and HTTP clients: a METASET
  /// that observed a vector cannot be replayed before that vector's record.
  std::mutex* write_serialization_gate = nullptr;

  /// Single source of truth for cache ownership, publication, and tuning.
  cache::SimilarityCacheController* cache_controller = nullptr;

  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

}  // namespace nvecd::server

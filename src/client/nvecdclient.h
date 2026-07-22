/**
 * @file nvecdclient.h
 * @brief C++ Client library for nvecd
 *
 * Reference: ../mygram-db/src/client/mygramclient.h
 * Reusability: 85% (connection management, PIMPL pattern, common commands)
 * Adapted for: nvecd vector search operations (EVENT, VECSET, SIM, SIMV)
 *
 * This library provides a high-level C++ interface for connecting to and
 * querying nvecd servers. It supports all nvecd protocol commands including
 * EVENT, VECSET, SIM, SIMV, INFO, DUMP, and DEBUG.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::client {

namespace detail {

/**
 * @brief Determine whether a received response is complete (framing).
 *
 * The nvecd wire protocol uses line-delimited text responses. Most responses
 * are a single line terminated by a newline. SIM/SIMV searches, however, send a
 * multi-line response framed by a header:
 *
 * @code
 *   OK RESULTS <count>\r\n
 *   <id1> <score1>\r\n
 *   ...
 *   <idN> <scoreN>\r\n
 * @endcode
 *
 * A single recv() may return only part of such a response (e.g. just the header
 * line), or several responses at once. Using "buffer ends with a newline" as the
 * completion signal therefore truncates SIM/SIMV results. This helper inspects
 * the accumulated buffer and reports completion using length-aware framing:
 * when the buffer starts with "OK RESULTS <count>", it is complete only once
 * <count> + 1 newline-terminated lines have been received; otherwise a single
 * newline-terminated line marks completion.
 *
 * @param buffer Accumulated bytes received so far.
 * @return true if @p buffer contains at least one complete response.
 */
bool IsResponseComplete(const std::string& buffer);

/**
 * @brief Return the byte length of the first complete response in @p buffer.
 *
 * A receive can contain a complete response followed by bytes from the next
 * response. Callers use this length to preserve trailing bytes for the next
 * command instead of letting them contaminate the current response.
 */
std::optional<size_t> CompleteResponseLength(const std::string& buffer, bool requires_end_terminator = false);

}  // namespace detail

/**
 * @brief Similarity search result item
 */
struct SimResultItem {
  std::string id;     ///< Document/vector ID
  float score{0.0F};  ///< Similarity score

  SimResultItem() = default;
  SimResultItem(std::string id_value, float score_value) : id(std::move(id_value)), score(score_value) {}
};

/**
 * @brief Similarity search response
 */
struct SimResponse {
  std::vector<SimResultItem> results;  ///< Search results (sorted by score descending)
  std::string mode;                    ///< Search mode used (events/vectors/fusion)

  SimResponse() = default;
};

/**
 * @brief Optional search parameters for SIM/SIMV.
 *
 * These map directly to the optional tokens of the SIM/SIMV wire commands:
 * @code
 *   SIM  <id>   <top_k> [using=<mode>] [filter=<expr>] [min_score=<f>] [adaptive=on|off]
 *   SIMV <top_k> [filter=<expr>] [min_score=<f>] <f1> <f2> ...
 * @endcode
 * Empty/unset fields are omitted from the command, preserving server defaults.
 * Note: @c adaptive applies only to fusion-mode SIM (ignored by SIMV).
 */
struct SearchOptions {
  std::string filter;              ///< Metadata filter expression, e.g. "category:books"
  std::optional<float> min_score;  ///< Minimum score threshold (min_score=)
  std::optional<bool> adaptive;    ///< Adaptive fusion toggle (adaptive=on|off); SIM fusion only
};

/**
 * @brief Server information
 *
 * Field names mirror the keys emitted by the server's INFO command
 * (see src/server/handlers/info_handler.cpp).
 */
struct ServerInfo {
  std::string version;
  uint64_t uptime_seconds = 0;
  uint64_t total_commands_processed = 0;  ///< INFO key: total_commands_processed
  uint64_t failed_commands = 0;           ///< INFO key: failed_commands
  uint64_t total_connections = 0;         ///< INFO key: total_connections_received
  uint64_t active_connections = 0;        ///< INFO key: active_connections
  uint64_t event_count = 0;               ///< Total events stored (INFO key: event_count)
  uint64_t vector_count = 0;              ///< Total vectors stored (INFO key: vector_count)
  uint64_t id_count = 0;                  ///< Distinct co-occurrence IDs (INFO key: id_count)
  uint64_t ctx_count = 0;                 ///< Distinct event contexts (INFO key: ctx_count)
};

/**
 * @brief Client configuration
 */
// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers) - Default nvecd client
// settings
struct ClientConfig {
  std::string host = "127.0.0.1";     ///< Server hostname
  uint16_t port = 11017;              ///< Default port for nvecd protocol
  uint32_t timeout_ms = 5000;         ///< Default timeout in milliseconds
  uint32_t recv_buffer_size = 65536;  ///< Default buffer size (64KB)
  std::string unix_socket_path;       ///< Unix socket path (empty = use TCP)
};
// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

/**
 * @brief nvecd client
 *
 * This class provides a thread-safe client for nvecd. Each instance
 * maintains a single TCP connection to the server.
 *
 * Example usage:
 * @code
 *   ClientConfig config;
 *   config.host = "localhost";
 *   config.port = 11017;
 *
 *   NvecdClient client(config);
 *   auto conn_result = client.Connect();
 *   if (!conn_result) {
 *     std::cerr << "Connection failed: " << conn_result.error().message() << std::endl;
 *     return;
 *   }
 *
 *   // Register event
 *   client.Event("ctx123", "vec456", 95);
 *
 *   // Register vector
 *   std::vector<float> vec = {0.1f, 0.2f, 0.3f, ...};
 *   client.Vecset("vec456", vec);
 *
 *   // Search by ID
 *   auto result = client.Sim("vec456", 10, "fusion");
 *   if (!result) {
 *     std::cerr << "Search failed: " << result.error().message() << std::endl;
 *   } else {
 *     auto& resp = *result;
 *     for (const auto& item : resp.results) {
 *       std::cout << item.id << " " << item.score << "\n";
 *     }
 *   }
 * @endcode
 */
class NvecdClient {
 public:
  /**
   * @brief Construct client with configuration
   * @param config Client configuration
   */
  explicit NvecdClient(ClientConfig config);

  /**
   * @brief Destructor - automatically disconnects
   */
  ~NvecdClient();

  // Non-copyable
  NvecdClient(const NvecdClient&) = delete;
  NvecdClient& operator=(const NvecdClient&) = delete;

  // Movable
  NvecdClient(NvecdClient&&) noexcept;
  NvecdClient& operator=(NvecdClient&&) noexcept;

  /**
   * @brief Connect to nvecd server
   * @return Expected<void, Error> - success or error
   */
  nvecd::utils::Expected<void, nvecd::utils::Error> Connect();

  /**
   * @brief Disconnect from server
   */
  void Disconnect();

  /**
   * @brief Check if connected to server
   * @return true if connected
   */
  [[nodiscard]] bool IsConnected() const;

  //
  // nvecd-specific commands
  //

  /**
   * @brief Register event (EVENT command)
   *
   * @param ctx Context ID
   * @param type Event type ("ADD", "SET", or "DEL")
   * @param id Document/vector ID
   * @param score Event score (0-100) - ignored for DEL type
   * @return Expected<void, Error>
   */
  nvecd::utils::Expected<void, nvecd::utils::Error> Event(const std::string& ctx, const std::string& type,
                                                          const std::string& id, int score = 0) const;

  /**
   * @brief Register vector (VECSET command)
   *
   * @param id Vector ID
   * @param vector Vector data (dimension must match server config)
   * @return Expected<void, Error>
   */
  nvecd::utils::Expected<void, nvecd::utils::Error> Vecset(const std::string& id,
                                                           const std::vector<float>& vector) const;

  /**
   * @brief Delete a vector (VECDEL command)
   *
   * @param id Vector ID
   * @return Expected<void, Error>
   */
  nvecd::utils::Expected<void, nvecd::utils::Error> Vecdel(const std::string& id) const;

  /**
   * @brief Attach metadata to an existing item (METASET command)
   *
   * Sends @c "METASET <id> <key:value[,key:value...]>". The item must already
   * exist via Vecset(). Stored metadata enables filtered SIM/SIMV queries.
   *
   * @param id Item ID (must already exist in the vector store)
   * @param metadata Metadata expression, e.g. "category:books,active:true"
   * @return Expected<void, Error>
   */
  nvecd::utils::Expected<void, nvecd::utils::Error> Metaset(const std::string& id, const std::string& metadata) const;

  /**
   * @brief Similarity search by ID (SIM command)
   *
   * @param id Document/vector ID
   * @param top_k Number of results to return (default: 10)
   * @param mode Search mode: "events", "vectors", or "fusion" (default: "fusion")
   * @param options Optional filter/min_score/adaptive parameters
   * @return Expected<SimResponse, Error>
   */
  nvecd::utils::Expected<SimResponse, nvecd::utils::Error> Sim(
      const std::string& id,
      uint32_t top_k = 10,  // NOLINT(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
                            // - Default result limit
      const std::string& mode = "fusion", const SearchOptions& options = {}) const;

  /**
   * @brief Similarity search by vector (SIMV command)
   *
   * @param vector Query vector
   * @param top_k Number of results to return (default: 10)
   * @param mode Search mode: "vectors" only (default: "vectors")
   * @param options Optional filter/min_score parameters (adaptive is ignored)
   * @return Expected<SimResponse, Error>
   */
  nvecd::utils::Expected<SimResponse, nvecd::utils::Error> Simv(
      const std::vector<float>& vector,
      uint32_t top_k = 10,  // NOLINT(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
                            // - Default result limit
      const std::string& mode = "vectors", const SearchOptions& options = {}) const;

  //
  // MygramDB-compatible commands
  //

  /**
   * @brief Authenticate the connection (AUTH command)
   *
   * Required when the server is configured with @c security.requirepass. Must be
   * called after Connect() and before issuing write/admin commands. Read
   * commands work without authentication.
   *
   * @param password Server password
   * @return Expected<void, Error> - success, or an error on invalid password
   */
  nvecd::utils::Expected<void, nvecd::utils::Error> Auth(const std::string& password) const;

  /**
   * @brief Get server information (INFO command)
   * @return Expected<ServerInfo, Error>
   */
  nvecd::utils::Expected<ServerInfo, nvecd::utils::Error> Info() const;

  /**
   * @brief Get server configuration (CONFIG SHOW command)
   * @return Expected<std::string, Error>
   */
  nvecd::utils::Expected<std::string, nvecd::utils::Error> GetConfig() const;

  /**
   * @brief Save snapshot to disk (DUMP SAVE command)
   * @param filepath Optional filepath (empty for default)
   * @return Expected<std::string, Error> - saved filepath or error
   */
  nvecd::utils::Expected<std::string, nvecd::utils::Error> Save(const std::string& filepath = "") const;

  /**
   * @brief Load snapshot from disk (DUMP LOAD command)
   * @param filepath Snapshot filepath
   * @return Expected<std::string, Error> - loaded filepath or error
   */
  nvecd::utils::Expected<std::string, nvecd::utils::Error> Load(const std::string& filepath) const;

  /**
   * @brief Verify snapshot integrity (DUMP VERIFY command)
   * @param filepath Snapshot filepath
   * @return Expected<std::string, Error> - verification result or error
   */
  nvecd::utils::Expected<std::string, nvecd::utils::Error> Verify(const std::string& filepath) const;

  /**
   * @brief Get snapshot metadata (DUMP INFO command)
   * @param filepath Snapshot filepath
   * @return Expected<std::string, Error> - snapshot metadata or error
   */
  nvecd::utils::Expected<std::string, nvecd::utils::Error> DumpInfo(const std::string& filepath) const;

  /**
   * @brief Query background snapshot status (DUMP STATUS command)
   * @return Expected<std::string, Error> - raw status block or error
   */
  nvecd::utils::Expected<std::string, nvecd::utils::Error> DumpStatus() const;

  /**
   * @brief Get cache statistics (CACHE STATS command)
   * @return Expected<std::string, Error> - raw stats block or error
   */
  nvecd::utils::Expected<std::string, nvecd::utils::Error> CacheStats() const;

  /**
   * @brief Clear all cache entries (CACHE CLEAR command)
   * @return Expected<void, Error>
   */
  nvecd::utils::Expected<void, nvecd::utils::Error> CacheClear() const;

  /**
   * @brief Enable the query result cache (CACHE ENABLE command)
   * @return Expected<void, Error>
   */
  nvecd::utils::Expected<void, nvecd::utils::Error> CacheEnable() const;

  /**
   * @brief Disable the query result cache (CACHE DISABLE command)
   * @return Expected<void, Error>
   */
  nvecd::utils::Expected<void, nvecd::utils::Error> CacheDisable() const;

  /**
   * @brief Enable debug mode for this connection (DEBUG ON command)
   * @return Expected<void, Error>
   */
  nvecd::utils::Expected<void, nvecd::utils::Error> EnableDebug() const;

  /**
   * @brief Disable debug mode for this connection (DEBUG OFF command)
   * @return Expected<void, Error>
   */
  nvecd::utils::Expected<void, nvecd::utils::Error> DisableDebug() const;

  /**
   * @brief Send raw command to server
   *
   * This is a low-level interface for sending custom commands.
   * Most users should use the higher-level methods instead.
   *
   * @param command Command string (without \r\n terminator)
   * @return Expected<std::string, Error>
   */
  nvecd::utils::Expected<std::string, nvecd::utils::Error> SendCommand(const std::string& command) const;

 private:
  class Impl;  // Forward declaration for PIMPL
  mutable std::unique_ptr<Impl> impl_;
};

}  // namespace nvecd::client

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
 * @brief Server information
 */
struct ServerInfo {
  std::string version;
  uint64_t uptime_seconds = 0;
  uint64_t total_requests = 0;
  uint64_t active_connections = 0;
  uint64_t event_count = 0;            ///< Total events stored
  uint64_t vector_count = 0;           ///< Total vectors stored
  uint64_t co_occurrence_entries = 0;  ///< Co-occurrence index entries
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
   * @brief Similarity search by ID (SIM command)
   *
   * @param id Document/vector ID
   * @param top_k Number of results to return (default: 10)
   * @param mode Search mode: "events", "vectors", or "fusion" (default: "fusion")
   * @return Expected<SimResponse, Error>
   */
  nvecd::utils::Expected<SimResponse, nvecd::utils::Error> Sim(
      const std::string& id,
      uint32_t top_k = 10,  // NOLINT(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
                            // - Default result limit
      const std::string& mode = "fusion") const;

  /**
   * @brief Similarity search by vector (SIMV command)
   *
   * @param vector Query vector
   * @param top_k Number of results to return (default: 10)
   * @param mode Search mode: "vectors" only (default: "vectors")
   * @return Expected<SimResponse, Error>
   */
  nvecd::utils::Expected<SimResponse, nvecd::utils::Error> Simv(
      const std::vector<float>& vector,
      uint32_t top_k = 10,  // NOLINT(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
                            // - Default result limit
      const std::string& mode = "vectors") const;

  //
  // MygramDB-compatible commands
  //

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

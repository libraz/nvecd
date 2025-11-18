/**
 * @file nvecd_server.h
 * @brief Main nvecd server
 *
 * Reference: ../mygram-db/src/server/tcp_server.h
 * Reusability: 70% (similar structure, simplified for nvecd)
 */

#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "cache/similarity_cache.h"
#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/connection_acceptor.h"
#include "server/http_server.h"
#include "server/request_dispatcher.h"
#include "server/server_types.h"
#include "server/thread_pool.h"
#include "similarity/similarity_engine.h"
#include "utils/error.h"
#include "utils/expected.h"
#include "vectors/vector_store.h"

namespace nvecd::server {

/**
 * @brief Main nvecd TCP server
 *
 * Coordinates all server components:
 * - EventStore: Event history management
 * - CoOccurrenceIndex: Co-occurrence tracking
 * - VectorStore: Vector storage
 * - SimilarityEngine: Similarity search
 * - RequestDispatcher: Command routing
 * - ConnectionAcceptor: Network handling
 * - ThreadPool: Connection handling
 *
 * Thread-safety:
 * - Start/Stop methods are not thread-safe (call from main thread only)
 * - All other methods are thread-safe
 *
 * Example:
 * @code
 * config::Config config = LoadConfig("config.yaml");
 * NvecdServer server(config);
 * auto result = server.Start();
 * if (!result) {
 *   std::cerr << "Failed to start: " << result.error().message() << std::endl;
 *   return 1;
 * }
 * // ... server runs ...
 * server.Stop();
 * @endcode
 */
class NvecdServer {
 public:
  /**
   * @brief Construct nvecd server
   * @param config Server configuration
   */
  explicit NvecdServer(const config::Config& config);

  /**
   * @brief Destructor (calls Stop if running)
   */
  ~NvecdServer();

  // Non-copyable and non-movable
  NvecdServer(const NvecdServer&) = delete;
  NvecdServer& operator=(const NvecdServer&) = delete;
  NvecdServer(NvecdServer&&) = delete;
  NvecdServer& operator=(NvecdServer&&) = delete;

  /**
   * @brief Start server
   *
   * Initializes all components and starts accepting connections.
   *
   * @return Expected<void, Error> Success or error details
   */
  utils::Expected<void, utils::Error> Start();

  /**
   * @brief Stop server
   *
   * Stops accepting new connections and waits for existing connections
   * to complete (with timeout).
   */
  void Stop();

  /**
   * @brief Check if server is running
   */
  bool IsRunning() const { return running_.load(); }

  /**
   * @brief Get server port
   */
  uint16_t GetPort() const { return acceptor_ ? acceptor_->GetPort() : 0; }

  /**
   * @brief Get active connection count
   */
  size_t GetConnectionCount() const { return stats_.active_connections.load(); }

  /**
   * @brief Get total commands processed
   */
  uint64_t GetTotalCommands() const { return stats_.total_commands.load(); }

  /**
   * @brief Get server statistics
   */
  const ServerStats& GetStats() const { return stats_; }

 private:
  /**
   * @brief Initialize all server components
   * @return Expected<void, Error> Success or error
   */
  utils::Expected<void, utils::Error> InitializeComponents();

  /**
   * @brief Handle client connection
   * @param client_fd Client socket file descriptor
   */
  void HandleConnection(int client_fd);

  /**
   * @brief Process single request
   * @param request Request string
   * @param conn_ctx Connection context
   * @return Response string
   */
  std::string ProcessRequest(const std::string& request, ConnectionContext& conn_ctx);

  // Configuration
  config::Config config_;

  // Server state
  std::atomic<bool> running_{false};
  std::atomic<bool> shutdown_{false};  // Set to true when shutting down
  std::atomic<bool> loading_{false};
  std::atomic<bool> read_only_{false};  // Set to true during snapshot save

  // Statistics
  ServerStats stats_;

  // Core components (owned)
  std::unique_ptr<events::EventStore> event_store_;
  std::unique_ptr<events::CoOccurrenceIndex> co_index_;
  std::unique_ptr<vectors::VectorStore> vector_store_;
  std::unique_ptr<similarity::SimilarityEngine> similarity_engine_;
  std::unique_ptr<cache::SimilarityCache> cache_;

  // Handler context (must be declared before dispatcher_ to ensure proper initialization order)
  HandlerContext handler_ctx_{
      .event_store = nullptr,
      .co_index = nullptr,
      .vector_store = nullptr,
      .similarity_engine = nullptr,
      .cache = nullptr,
      .stats = stats_,
      .config = &config_,
      .loading = loading_,
      .read_only = read_only_,
      .dump_dir = "",  // Will be initialized from config in InitializeComponents
  };

  // Server components (owned)
  std::unique_ptr<RequestDispatcher> dispatcher_;
  std::unique_ptr<ThreadPool> thread_pool_;
  std::unique_ptr<ConnectionAcceptor> acceptor_;
  std::unique_ptr<HttpServer> http_server_;  // HTTP API server (optional)
};

}  // namespace nvecd::server

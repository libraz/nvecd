/**
 * @file http_server.h
 * @brief HTTP server for JSON API
 *
 * Reference: ../mygram-db/src/server/http_server.h
 * Reusability: 85% (infrastructure, health endpoints, CORS)
 * Adapted for: nvecd vector operations (EVENT, VECSET, SIM, SIMV)
 */

#pragma once

// Fix for httplib missing NI_MAXHOST on some platforms
#ifndef NI_MAXHOST
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NI_MAXHOST 1025
#endif

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config/config.h"
#include "server/server_types.h"
#include "utils/error.h"
#include "utils/expected.h"
#include "utils/network_utils.h"

namespace nvecd::server {

/**
 * @brief HTTP server configuration
 */
struct HttpServerConfig {
  std::string bind = "0.0.0.0";
  int port = 8081;
  int read_timeout_sec = 5;
  int write_timeout_sec = 5;
  bool enable_cors = false;
  std::string cors_allow_origin;
  std::vector<std::string> allow_cidrs;
};

/**
 * @brief HTTP server for JSON API
 *
 * Provides RESTful JSON API:
 * - POST /event - Register co-occurrence event
 * - POST /vecset - Register vector
 * - POST /sim - Similarity search by ID
 * - POST /simv - Similarity search by vector
 * - GET /info - Server information
 * - GET /health/* - Health check endpoints
 * - GET /config - Configuration summary
 * - POST /dump/* - Snapshot management
 * - POST /debug/on|off - Debug mode
 */
class HttpServer {
 public:
  /**
   * @brief Construct HTTP server
   * @param config Server configuration
   * @param handler_context Shared context with event/vector stores
   * @param full_config Full application configuration
   * @param loading Reference to loading flag (shared with TcpServer)
   * @param tcp_stats Optional pointer to TCP server's ServerStats (for /info)
   */
  HttpServer(HttpServerConfig config, HandlerContext* handler_context, const config::Config* full_config = nullptr,
             std::atomic<bool>* loading = nullptr, ServerStats* tcp_stats = nullptr);

  ~HttpServer();

  // Non-copyable and non-movable (manages server thread)
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&&) = delete;
  HttpServer& operator=(HttpServer&&) = delete;

  /**
   * @brief Start server (non-blocking)
   * @return Expected<void, Error> - Success or error details
   */
  nvecd::utils::Expected<void, nvecd::utils::Error> Start();

  /**
   * @brief Stop server
   */
  void Stop();

  /**
   * @brief Check if server is running
   */
  bool IsRunning() const { return running_; }

  /**
   * @brief Get server port
   */
  int GetPort() const { return config_.port; }

  /**
   * @brief Get total requests handled
   */
  uint64_t GetTotalRequests() const { return stats_.total_commands.load(); }

  /**
   * @brief Get server statistics
   */
  const ServerStats& GetStats() const { return stats_; }

 private:
  HttpServerConfig config_;
  HandlerContext* handler_context_;

  std::atomic<bool> running_{false};

  // Statistics
  ServerStats stats_;

  std::unique_ptr<httplib::Server> server_;
  std::unique_ptr<std::thread> server_thread_;

  const config::Config* full_config_;

  std::vector<utils::CIDR> parsed_allow_cidrs_;
  std::atomic<bool>* loading_;  // Shared loading flag (owned by TcpServer)
  ServerStats* tcp_stats_;      // Pointer to TCP server's statistics (for /info)

  /**
   * @brief Setup routes
   */
  void SetupRoutes();

  /**
   * @brief Setup CIDR-based access control
   */
  void SetupAccessControl();

  /**
   * @brief Setup CORS middleware
   */
  void SetupCors();

  //
  // Request handlers: nvecd-specific operations
  //

  /**
   * @brief Handle POST /event
   */
  void HandleEvent(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /vecset
   */
  void HandleVecset(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /sim
   */
  void HandleSim(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /simv
   */
  void HandleSimv(const httplib::Request& req, httplib::Response& res);

  //
  // Request handlers: MygramDB-compatible operations
  //

  /**
   * @brief Handle GET /info
   */
  void HandleInfo(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle GET /health (legacy endpoint)
   */
  void HandleHealth(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle GET /health/live (liveness probe)
   */
  void HandleHealthLive(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle GET /health/ready (readiness probe)
   */
  void HandleHealthReady(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle GET /health/detail (detailed health status)
   */
  void HandleHealthDetail(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle GET /config
   */
  void HandleConfig(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /dump/save
   */
  void HandleDumpSave(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /dump/load
   */
  void HandleDumpLoad(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /dump/verify
   */
  void HandleDumpVerify(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /dump/info
   */
  void HandleDumpInfo(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /debug/on
   */
  void HandleDebugOn(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /debug/off
   */
  void HandleDebugOff(const httplib::Request& req, httplib::Response& res);

  //
  // Utility methods
  //

  /**
   * @brief Send JSON response
   */
  static void SendJson(httplib::Response& res, int status_code, const nlohmann::json& body);

  /**
   * @brief Send error response
   */
  static void SendError(httplib::Response& res, int status_code, const std::string& message);
};

}  // namespace nvecd::server

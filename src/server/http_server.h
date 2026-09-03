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

#include <atomic>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "config/config.h"
#include "server/command_types.h"
#include "server/rate_limiter.h"
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
  std::string requirepass;  ///< Required password for write/admin endpoints (empty = no auth)
  /// Maximum accepted HTTP request body size in bytes. Requests with a larger
  /// payload are rejected by httplib before reaching a handler. Defaults to 8MB.
  size_t max_payload_bytes = 8UL * 1024UL * 1024UL;
  size_t worker_threads = 8;
  size_t max_queued_connections = 128;
  size_t max_connections = 256;         ///< 0 means unlimited.
  size_t max_connections_per_ip = 100;  ///< 0 means unlimited.
};

/**
 * @brief HTTP server for JSON API
 *
 * Provides RESTful JSON API:
 * - POST /event - Register co-occurrence event
 * - POST /vecset - Register vector
 * - DELETE /vecset - Delete vector by JSON {"id":"..."}
 * - POST /sim - Similarity search by ID
 * - POST /simv - Similarity search by vector
 * - GET /info - Server information
 * - GET /health/... - Health check endpoints
 * - GET /config - Configuration summary
 * - POST /dump/... - Snapshot management
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
             std::atomic<bool>* loading = nullptr, ServerStats* tcp_stats = nullptr,
             RateLimiter* rate_limiter = nullptr);

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
   * @brief Get the port the listening socket is bound to
   *
   * When the configuration requests port 0 the operating system picks an
   * ephemeral port, so the configured value cannot answer this question. The
   * reported value comes from the listening socket itself (getsockname), and
   * is therefore only meaningful while the server is bound; before Start()
   * succeeds and after Stop() it falls back to the configured value.
   */
  int GetPort() const { return bound_port_.load(std::memory_order_acquire); }

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

  /// Port reported by the listening socket; equals config_.port while unbound.
  std::atomic<int> bound_port_;

  // Statistics
  ServerStats stats_;

  std::unique_ptr<httplib::Server> server_;
  std::unique_ptr<std::thread> server_thread_;
  std::mutex lifecycle_mutex_;

  const config::Config* full_config_;

  std::vector<utils::CIDR> parsed_allow_cidrs_;
  std::atomic<bool>* loading_;  // Shared loading flag (owned by TcpServer)
  ServerStats* tcp_stats_;      // Pointer to TCP server's statistics (for /info)
  /// Shared with TCP when constructed by NvecdServer; owned fallback supports
  /// standalone HTTP use while preserving the configured limiter semantics.
  std::unique_ptr<RateLimiter> owned_rate_limiter_;
  RateLimiter* rate_limiter_ = nullptr;

  /// HTTP verb a route is registered under.
  enum class RouteMethod : uint8_t { kGet, kPost, kDelete };

  /// Member handler invoked once a route has been admitted.
  using RouteHandler = void (HttpServer::*)(const httplib::Request&, httplib::Response&);

  /**
   * @brief Setup routes
   */
  void SetupRoutes();

  /**
   * @brief Register a route that mirrors a TCP command
   *
   * Authorization and statistics are the wrapper's responsibility, not the
   * handler's. The required privilege is derived from GetCommandPrivilege(),
   * so both surfaces read the same authority and a route cannot be opened by
   * forgetting a gate line: registering it without naming a command type does
   * not compile. The request is accounted through CommandStatsScope on every
   * exit path, including the 401.
   *
   * @param method HTTP verb
   * @param path Route path
   * @param command TCP command this route is the HTTP form of
   * @param handler Member handler to invoke once admitted
   */
  void RegisterRoute(RouteMethod method, const std::string& path, CommandType command, RouteHandler handler);

  /**
   * @brief Register an endpoint that has no TCP command counterpart
   *
   * Used for the health probes and the Prometheus scrape endpoint: they are
   * not protocol commands, so they are deliberately neither gated nor counted
   * in the command statistics. The separate entry point makes that exemption
   * an explicit declaration rather than an omission.
   */
  void RegisterUncountedRoute(RouteMethod method, const std::string& path, RouteHandler handler);

  /// Bind an already-wrapped handler to @p path under @p method.
  void RegisterHandler(RouteMethod method, const std::string& path, httplib::Server::Handler handler);

  /**
   * @brief Statistics instance both surfaces share when wired together
   */
  ServerStats& EffectiveStats();
  const ServerStats& EffectiveStats() const;

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
   * @brief Handle DELETE /vecset
   */
  void HandleVecdel(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /metaset
   */
  void HandleMetaset(const httplib::Request& req, httplib::Response& res);

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
   * @brief Handle GET /metrics (Prometheus metrics)
   */
  void HandleMetrics(const httplib::Request& req, httplib::Response& res);

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

  /**
   * @brief Handle GET /cache/stats
   */
  void HandleCacheStats(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /cache/clear
   */
  void HandleCacheClear(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /cache/enable
   */
  void HandleCacheEnable(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle POST /cache/disable
   */
  void HandleCacheDisable(const httplib::Request& req, httplib::Response& res);

  /**
   * @brief Handle GET /dump/status
   */
  void HandleDumpStatus(const httplib::Request& req, httplib::Response& res);

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

  /**
   * @brief Check whether a request carries the configured credential
   *
   * When no password is configured (requirepass empty) every request is
   * authorized. Otherwise the request must present a matching credential via
   * the Authorization header, either "Bearer <password>" or
   * "Basic base64(user:<password>)" (the username is ignored, mirroring TCP
   * AUTH which only compares the password).
   *
   * Called only from the route wrapper: handler bodies must not gate
   * themselves, because a gate that lives in a handler body can be omitted.
   *
   * @param req Incoming request
   * @return true if authorized
   */
  bool IsAuthorized(const httplib::Request& req) const;
};

}  // namespace nvecd::server

/**
 * @file http_server.cpp
 * @brief HTTP server implementation
 *
 * Reference: ../mygram-db/src/server/http_server.cpp
 * Reusability: 80% (infrastructure, health endpoints)
 * Adapted for: nvecd vector operations
 */

#include "server/http_server.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <sstream>

#include "cache/similarity_cache.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/command_parser.h"
#include "server/request_dispatcher.h"
#include "similarity/similarity_engine.h"
#include "utils/memory_utils.h"
#include "utils/network_utils.h"
#include "utils/string_utils.h"
#include "utils/structured_log.h"
#include "vectors/vector_store.h"
#include "version.h"

// Fix for httplib missing NI_MAXHOST on some platforms
#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

using json = nlohmann::json;

namespace nvecd::server {

namespace {
// HTTP status codes
constexpr int kHttpOk = 200;
constexpr int kHttpNoContent = 204;
constexpr int kHttpBadRequest = 400;
constexpr int kHttpForbidden = 403;
constexpr int kHttpNotFound = 404;
constexpr int kHttpInternalServerError = 500;
constexpr int kHttpServiceUnavailable = 503;

// Server startup delay (milliseconds)
constexpr int kStartupDelayMs = 100;

std::vector<utils::CIDR> ParseAllowCidrs(const std::vector<std::string>& allow_cidrs) {
  std::vector<utils::CIDR> parsed;
  parsed.reserve(allow_cidrs.size());

  for (const auto& cidr_str : allow_cidrs) {
    auto cidr = utils::CIDR::Parse(cidr_str);
    if (!cidr) {
      nvecd::utils::StructuredLog()
          .Event("server_warning")
          .Field("type", "invalid_cidr_entry")
          .Field("cidr", cidr_str)
          .Warn();
      continue;
    }
    parsed.push_back(*cidr);
  }

  return parsed;
}
}  // namespace

HttpServer::HttpServer(HttpServerConfig config, HandlerContext* handler_context, const config::Config* full_config,
                       std::atomic<bool>* loading, ServerStats* tcp_stats)
    : config_(std::move(config)),
      handler_context_(handler_context),
      full_config_(full_config),
      loading_(loading),
      tcp_stats_(tcp_stats) {
  parsed_allow_cidrs_ = ParseAllowCidrs(config_.allow_cidrs);

  server_ = std::make_unique<httplib::Server>();

  // Set timeouts
  server_->set_read_timeout(config_.read_timeout_sec, 0);
  server_->set_write_timeout(config_.write_timeout_sec, 0);

  // Setup network ACL before registering routes
  SetupAccessControl();

  // Setup routes
  SetupRoutes();

  // Setup CORS if enabled
  if (config_.enable_cors) {
    SetupCors();
  }
}

HttpServer::~HttpServer() {
  Stop();
}

void HttpServer::SetupRoutes() {
  // nvecd-specific operations
  server_->Post("/event", [this](const httplib::Request& req, httplib::Response& res) { HandleEvent(req, res); });
  server_->Post("/vecset", [this](const httplib::Request& req, httplib::Response& res) { HandleVecset(req, res); });
  server_->Post("/sim", [this](const httplib::Request& req, httplib::Response& res) { HandleSim(req, res); });
  server_->Post("/simv", [this](const httplib::Request& req, httplib::Response& res) { HandleSimv(req, res); });

  // MygramDB-compatible operations
  server_->Get("/info", [this](const httplib::Request& req, httplib::Response& res) { HandleInfo(req, res); });

  // Health check endpoints
  server_->Get("/health", [this](const httplib::Request& req, httplib::Response& res) { HandleHealth(req, res); });
  server_->Get("/health/live",
               [this](const httplib::Request& req, httplib::Response& res) { HandleHealthLive(req, res); });
  server_->Get("/health/ready",
               [this](const httplib::Request& req, httplib::Response& res) { HandleHealthReady(req, res); });
  server_->Get("/health/detail",
               [this](const httplib::Request& req, httplib::Response& res) { HandleHealthDetail(req, res); });

  // Configuration
  server_->Get("/config", [this](const httplib::Request& req, httplib::Response& res) { HandleConfig(req, res); });

  // Metrics
  server_->Get("/metrics", [this](const httplib::Request& req, httplib::Response& res) { HandleMetrics(req, res); });

  // Cache management
  server_->Get("/cache/stats",
               [this](const httplib::Request& req, httplib::Response& res) { HandleCacheStats(req, res); });
  server_->Post("/cache/clear",
                [this](const httplib::Request& req, httplib::Response& res) { HandleCacheClear(req, res); });

  // Snapshot management
  server_->Post("/dump/save",
                [this](const httplib::Request& req, httplib::Response& res) { HandleDumpSave(req, res); });
  server_->Post("/dump/load",
                [this](const httplib::Request& req, httplib::Response& res) { HandleDumpLoad(req, res); });
  server_->Post("/dump/verify",
                [this](const httplib::Request& req, httplib::Response& res) { HandleDumpVerify(req, res); });
  server_->Post("/dump/info",
                [this](const httplib::Request& req, httplib::Response& res) { HandleDumpInfo(req, res); });

  // Debug mode
  server_->Post("/debug/on", [this](const httplib::Request& req, httplib::Response& res) { HandleDebugOn(req, res); });
  server_->Post("/debug/off",
                [this](const httplib::Request& req, httplib::Response& res) { HandleDebugOff(req, res); });
}

void HttpServer::SetupAccessControl() {
  server_->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
    if (utils::IsIPAllowed(req.remote_addr, parsed_allow_cidrs_)) {
      return httplib::Server::HandlerResponse::Unhandled;
    }

    ;
    const auto& remote = req.remote_addr.empty() ? std::string("<unknown>") : req.remote_addr;
    nvecd::utils::StructuredLog()
        .Event("server_warning")
        .Field("type", "http_request_rejected_acl")
        .Field("remote_addr", remote)
        .Warn();
    SendError(res, kHttpForbidden, "Access denied by network.allow_cidrs");
    return httplib::Server::HandlerResponse::Handled;
  });
}

void HttpServer::SetupCors() {
  const std::string allow_origin = config_.cors_allow_origin.empty() ? "null" : config_.cors_allow_origin;

  // CORS preflight
  server_->Options(".*", [allow_origin](const httplib::Request& /*req*/, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", allow_origin);
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
    res.status = kHttpNoContent;
  });

  // Add CORS headers to all responses
  server_->set_post_routing_handler([allow_origin](const httplib::Request& /*req*/, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", allow_origin);
  });
}

nvecd::utils::Expected<void, nvecd::utils::Error> HttpServer::Start() {
  using nvecd::utils::ErrorCode;
  using nvecd::utils::MakeError;
  using nvecd::utils::MakeUnexpected;

  if (running_) {
    auto error = MakeError(ErrorCode::kNetworkAlreadyRunning, "HTTP server already running");
    nvecd::utils::StructuredLog()
        .Event("server_error")
        .Field("operation", "http_server_start")
        .Field("error", error.to_string())
        .Error();
    return MakeUnexpected(error);
  }

  // Set running flag before starting thread to avoid race condition
  running_ = true;

  // Store error from thread (if any)
  std::string thread_error;
  std::mutex error_mutex;

  // Start server in separate thread
  server_thread_ = std::make_unique<std::thread>([this, &thread_error, &error_mutex]() {
    spdlog::info("Starting HTTP server on {}:{}", config_.bind, config_.port);

    if (!server_->listen(config_.bind, config_.port)) {
      std::lock_guard<std::mutex> lock(error_mutex);
      thread_error = "Failed to bind to " + config_.bind + ":" + std::to_string(config_.port);
      nvecd::utils::StructuredLog()
          .Event("server_error")
          .Field("operation", "http_server_listen")
          .Field("bind", config_.bind)
          .Field("port", static_cast<uint64_t>(config_.port))
          .Field("error", thread_error)
          .Error();
      running_ = false;
      return;
    }
  });

  // Wait a bit for server to start
  std::this_thread::sleep_for(std::chrono::milliseconds(kStartupDelayMs));

  if (!running_) {
    if (server_thread_ && server_thread_->joinable()) {
      server_thread_->join();
    }
    std::lock_guard<std::mutex> lock(error_mutex);
    auto error =
        MakeError(ErrorCode::kNetworkBindFailed, thread_error.empty() ? "Failed to start HTTP server" : thread_error);
    return MakeUnexpected(error);
  }

  spdlog::info("HTTP server started successfully on {}:{}", config_.bind, config_.port);
  return {};
}

void HttpServer::Stop() {
  if (!running_) {
    return;
  }

  spdlog::info("Stopping HTTP server...");
  running_ = false;

  if (server_) {
    server_->stop();
  }

  if (server_thread_ && server_thread_->joinable()) {
    server_thread_->join();
  }

  spdlog::info("HTTP server stopped");
}

void HttpServer::SendJson(httplib::Response& res, int status_code, const nlohmann::json& body) {
  res.status = status_code;
  res.set_content(body.dump(), "application/json");
}

void HttpServer::SendError(httplib::Response& res, int status_code, const std::string& message) {
  json error_obj;
  error_obj["error"] = message;
  SendJson(res, status_code, error_obj);
}

//
// Health check handlers (MygramDB-compatible, Kubernetes-ready)
// Reference: ../mygram-db/src/server/http_server.cpp (lines 974-1098)
// Reusability: 95%
//

void HttpServer::HandleHealth(const httplib::Request& /*req*/, httplib::Response& res) {
  ;

  json response;
  response["status"] = "ok";
  response["timestamp"] =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  SendJson(res, kHttpOk, response);
}

void HttpServer::HandleHealthLive(const httplib::Request& /*req*/, httplib::Response& res) {
  ;

  // Liveness probe: Always return 200 OK if the process is running
  // This is used by orchestrators (Kubernetes, Docker) to detect deadlocks
  json response;
  response["status"] = "alive";
  response["timestamp"] =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  SendJson(res, kHttpOk, response);
}

void HttpServer::HandleHealthReady(const httplib::Request& /*req*/, httplib::Response& res) {
  ;

  // Readiness probe: Return 200 OK if ready to accept traffic, 503 otherwise
  bool is_ready = (loading_ == nullptr || !loading_->load());

  json response;
  if (is_ready) {
    response["status"] = "ready";
    response["loading"] = false;
    response["timestamp"] =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    SendJson(res, kHttpOk, response);
  } else {
    response["status"] = "not_ready";
    response["loading"] = true;
    response["reason"] = "Server is loading";
    response["timestamp"] =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    SendJson(res, kHttpServiceUnavailable, response);
  }
}

void HttpServer::HandleHealthDetail(const httplib::Request& /*req*/, httplib::Response& res) {
  ;

  // Detailed health: Return comprehensive component status
  json response;

  // Overall status
  bool is_loading = (loading_ != nullptr && loading_->load());
  response["status"] = is_loading ? "degraded" : "healthy";
  response["timestamp"] =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  // Calculate uptime (simple ServerStats doesn't track uptime)
  response["uptime_seconds"] = 0;

  // Components status
  json components;

  // Server component
  json server_comp;
  server_comp["status"] = is_loading ? "loading" : "ready";
  server_comp["loading"] = is_loading;
  components["server"] = server_comp;

  // Event store component
  if (handler_context_ != nullptr && handler_context_->event_store) {
    json event_comp;
    event_comp["status"] = "ok";
    auto event_stats = handler_context_->event_store->GetStatistics();
    event_comp["contexts"] = event_stats.active_contexts;
    event_comp["total_events"] = event_stats.total_events;
    components["event_store"] = event_comp;
  }

  // Vector store component
  if (handler_context_ != nullptr && handler_context_->vector_store) {
    json vector_comp;
    vector_comp["status"] = "ok";
    auto vector_stats = handler_context_->vector_store->GetStatistics();
    vector_comp["vectors"] = vector_stats.vector_count;
    vector_comp["dimension"] = vector_stats.dimension;
    components["vector_store"] = vector_comp;
  }

  // Co-occurrence index component
  if (handler_context_ != nullptr && handler_context_->co_index) {
    json co_comp;
    co_comp["status"] = "ok";
    auto co_stats = handler_context_->co_index->GetStatistics();
    co_comp["tracked_ids"] = co_stats.tracked_ids;
    components["co_index"] = co_comp;
  }

  response["components"] = components;

  SendJson(res, kHttpOk, response);
}

//
// Info and Config handlers (MygramDB-compatible)
// Reference: ../mygram-db/src/server/http_server.cpp (lines 822-972)
// Reusability: 75% (adapted for nvecd stores)
//

void HttpServer::HandleInfo(const httplib::Request& /*req*/, httplib::Response& res) {
  // Increment request counter on the effective stats instance
  if (tcp_stats_ != nullptr) {
    ;
  } else {
    ;
  }

  try {
    json response;

    // Use TCP server's stats if available (includes all protocol stats), otherwise use HTTP-only stats
    const ServerStats& effective_stats = (tcp_stats_ != nullptr) ? *tcp_stats_ : stats_;

    // Server info
    response["server"] = "nvecd";
    response["version"] = nvecd::Version::String();
    response["uptime_seconds"] = 0;  // Simple ServerStats doesn't track uptime

    // Statistics (direct field access from simple ServerStats)
    response["total_requests"] = effective_stats.total_commands.load();
    response["total_commands_processed"] = effective_stats.total_commands.load();
    response["failed_commands"] = effective_stats.failed_commands.load();

    // Memory statistics
    size_t total_memory = 0;

    // Event store memory
    size_t event_memory = 0;
    if (handler_context_ != nullptr && handler_context_->event_store) {
      event_memory = handler_context_->event_store->MemoryUsage();
      total_memory += event_memory;
    }

    // Vector store memory
    size_t vector_memory = 0;
    if (handler_context_ != nullptr && handler_context_->vector_store) {
      vector_memory = handler_context_->vector_store->MemoryUsage();
      total_memory += vector_memory;
    }

    // Co-occurrence index memory
    size_t co_memory = 0;
    if (handler_context_ != nullptr && handler_context_->co_index) {
      co_memory = handler_context_->co_index->MemoryUsage();
      total_memory += co_memory;
    }

    // Memory tracking (simple ServerStats doesn't have UpdateMemoryUsage method)
    // Memory is tracked through store statistics instead

    json memory_obj;
    memory_obj["used_memory_bytes"] = total_memory;
    memory_obj["used_memory_human"] = utils::FormatBytes(total_memory);
    memory_obj["peak_memory_bytes"] = 0;  // Simple ServerStats doesn't track peak memory
    memory_obj["peak_memory_human"] = utils::FormatBytes(0);
    memory_obj["used_memory_events"] = utils::FormatBytes(event_memory);
    memory_obj["used_memory_vectors"] = utils::FormatBytes(vector_memory);
    memory_obj["used_memory_co_occurrence"] = utils::FormatBytes(co_memory);

    // System memory information
    auto sys_info = utils::GetSystemMemoryInfo();
    if (sys_info) {
      memory_obj["total_system_memory"] = sys_info->total_physical_bytes;
      memory_obj["total_system_memory_human"] = utils::FormatBytes(sys_info->total_physical_bytes);
      memory_obj["available_system_memory"] = sys_info->available_physical_bytes;
      memory_obj["available_system_memory_human"] = utils::FormatBytes(sys_info->available_physical_bytes);
      if (sys_info->total_physical_bytes > 0) {
        double usage_ratio = 1.0 - static_cast<double>(sys_info->available_physical_bytes) /
                                       static_cast<double>(sys_info->total_physical_bytes);
        memory_obj["system_memory_usage_ratio"] = usage_ratio;
      }
    }

    // Process memory information
    auto proc_info = utils::GetProcessMemoryInfo();
    if (proc_info) {
      memory_obj["process_rss"] = proc_info->rss_bytes;
      memory_obj["process_rss_human"] = utils::FormatBytes(proc_info->rss_bytes);
      memory_obj["process_rss_peak"] = proc_info->peak_rss_bytes;
      memory_obj["process_rss_peak_human"] = utils::FormatBytes(proc_info->peak_rss_bytes);
    }

    // Memory health status
    auto health = utils::GetMemoryHealthStatus();
    memory_obj["memory_health"] = utils::MemoryHealthStatusToString(health);

    response["memory"] = memory_obj;

    // Store statistics
    json stores_obj;

    if (handler_context_ != nullptr && handler_context_->event_store) {
      auto event_stats = handler_context_->event_store->GetStatistics();
      json event_obj;
      event_obj["contexts"] = event_stats.active_contexts;
      event_obj["total_events"] = event_stats.total_events;
      stores_obj["event_store"] = event_obj;
    }

    if (handler_context_ != nullptr && handler_context_->vector_store) {
      auto vector_stats = handler_context_->vector_store->GetStatistics();
      json vector_obj;
      vector_obj["vectors"] = vector_stats.vector_count;
      vector_obj["dimension"] = vector_stats.dimension;
      stores_obj["vector_store"] = vector_obj;
    }

    if (handler_context_ != nullptr && handler_context_->co_index) {
      auto co_stats = handler_context_->co_index->GetStatistics();
      json co_obj;
      co_obj["tracked_ids"] = co_stats.tracked_ids;
      stores_obj["co_index"] = co_obj;
    }

    response["stores"] = stores_obj;

    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

void HttpServer::HandleConfig(const httplib::Request& /*req*/, httplib::Response& res) {
  ;

  if (full_config_ == nullptr) {
    SendError(res, kHttpInternalServerError, "Configuration not available");
    return;
  }

  try {
    json response;

    // Network config summary (no bind/port exposure for security)
    json net_obj;
    net_obj["tcp_enabled"] = true;  // TCP is always enabled
    net_obj["http_enabled"] = full_config_->api.http.enable;
    net_obj["allow_cidrs_configured"] = !full_config_->network.allow_cidrs.empty();
    response["network"] = net_obj;

    // Event store config
    json events_obj;
    events_obj["ctx_buffer_size"] = full_config_->events.ctx_buffer_size;
    events_obj["decay_interval_sec"] = full_config_->events.decay_interval_sec;
    response["events"] = events_obj;

    // Vector store config
    json vectors_obj;
    vectors_obj["default_dimension"] = full_config_->vectors.default_dimension;
    response["vectors"] = vectors_obj;

    // Similarity config
    json similarity_obj;
    similarity_obj["default_top_k"] = full_config_->similarity.default_top_k;
    similarity_obj["fusion_alpha"] = full_config_->similarity.fusion_alpha;
    response["similarity"] = similarity_obj;

    response["notes"] = "Sensitive configuration values are redacted. Use CONFIG SHOW over TCP for full details.";

    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

//
// nvecd-specific operation handlers
// These delegate to RequestDispatcher for consistency with TCP protocol
//

void HttpServer::HandleEvent(const httplib::Request& req, httplib::Response& res) {
  ;

  // Check if server is loading
  if (loading_ != nullptr && loading_->load()) {
    SendError(res, kHttpServiceUnavailable, "Server is loading, please try again later");
    return;
  }

  try {
    // Parse JSON body
    json body = json::parse(req.body);

    // Validate required fields
    if (!body.contains("ctx") || !body.contains("id") || !body.contains("type")) {
      SendError(res, kHttpBadRequest, "Missing required fields: ctx, id, type");
      return;
    }

    std::string ctx = body["ctx"];
    std::string id = body["id"];
    std::string type_str = body["type"];

    // Parse event type
    events::EventType event_type;
    if (type_str == "ADD" || type_str == "add") {
      event_type = events::EventType::ADD;
      if (!body.contains("score")) {
        SendError(res, kHttpBadRequest, "ADD type requires 'score' field");
        return;
      }
    } else if (type_str == "SET" || type_str == "set") {
      event_type = events::EventType::SET;
      if (!body.contains("score")) {
        SendError(res, kHttpBadRequest, "SET type requires 'score' field");
        return;
      }
    } else if (type_str == "DEL" || type_str == "del") {
      event_type = events::EventType::DEL;
    } else {
      SendError(res, kHttpBadRequest, "Invalid type: " + type_str + " (must be ADD, SET, or DEL)");
      return;
    }

    int score = 0;
    if (event_type != events::EventType::DEL) {
      score = body["score"];
    }

    // Add event to event store
    if (handler_context_->event_store) {
      auto result = handler_context_->event_store->AddEvent(ctx, id, score, event_type);
      if (!result) {
        SendError(res, kHttpInternalServerError, result.error().message());
        return;
      }
    } else {
      SendError(res, kHttpInternalServerError, "Event store not initialized");
      return;
    }

    // Update co-occurrence index
    if (handler_context_->co_index) {
      auto events = handler_context_->event_store->GetEvents(ctx);
      handler_context_->co_index->UpdateFromEvents(ctx, events);
    }

    stats_.event_commands.fetch_add(1);
    stats_.total_commands.fetch_add(1);

    json response;
    response["status"] = "ok";
    SendJson(res, kHttpOk, response);

  } catch (const json::parse_error& e) {
    SendError(res, kHttpBadRequest, "Invalid JSON: " + std::string(e.what()));
  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

void HttpServer::HandleVecset(const httplib::Request& req, httplib::Response& res) {
  ;

  // Check if server is loading
  if (loading_ != nullptr && loading_->load()) {
    SendError(res, kHttpServiceUnavailable, "Server is loading, please try again later");
    return;
  }

  try {
    // Parse JSON body
    json body = json::parse(req.body);

    // Validate required fields
    if (!body.contains("id") || !body.contains("vector")) {
      SendError(res, kHttpBadRequest, "Missing required fields: id, vector");
      return;
    }

    std::string id = body["id"];
    std::vector<float> vector = body["vector"].get<std::vector<float>>();

    // Add vector to vector store
    if (handler_context_->vector_store) {
      auto result = handler_context_->vector_store->SetVector(id, vector);
      if (!result) {
        SendError(res, kHttpInternalServerError, result.error().message());
        return;
      }
    } else {
      SendError(res, kHttpInternalServerError, "Vector store not initialized");
      return;
    }

    stats_.vecset_commands.fetch_add(1);
    stats_.total_commands.fetch_add(1);

    json response;
    response["status"] = "ok";
    response["dimension"] = vector.size();
    SendJson(res, kHttpOk, response);

  } catch (const json::parse_error& e) {
    SendError(res, kHttpBadRequest, "Invalid JSON: " + std::string(e.what()));
  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

void HttpServer::HandleSim(const httplib::Request& req, httplib::Response& res) {
  ;

  // Check if server is loading
  if (loading_ != nullptr && loading_->load()) {
    SendError(res, kHttpServiceUnavailable, "Server is loading, please try again later");
    return;
  }

  try {
    // Parse JSON body
    json body = json::parse(req.body);

    // Validate required fields
    if (!body.contains("id")) {
      SendError(res, kHttpBadRequest, "Missing required field: id");
      return;
    }

    std::string id = body["id"];
    int top_k = body.value("top_k", handler_context_->config->similarity.default_top_k);
    std::string mode = body.value("mode", "fusion");

    // Check if similarity engine is initialized
    if (!handler_context_->similarity_engine) {
      SendError(res, kHttpInternalServerError, "Similarity engine not initialized");
      return;
    }

    // Call appropriate search method based on mode
    utils::Expected<std::vector<similarity::SimilarityResult>, utils::Error> result;
    if (mode == "events") {
      result = handler_context_->similarity_engine->SearchByIdEvents(id, top_k);
    } else if (mode == "vectors") {
      result = handler_context_->similarity_engine->SearchByIdVectors(id, top_k);
    } else if (mode == "fusion") {
      result = handler_context_->similarity_engine->SearchByIdFusion(id, top_k);
    } else {
      SendError(res, kHttpBadRequest, "Invalid mode. Must be one of: events, vectors, fusion");
      return;
    }

    if (!result) {
      if (result.error().code() == utils::ErrorCode::kVectorNotFound) {
        SendError(res, kHttpNotFound, result.error().message());
      } else {
        SendError(res, kHttpInternalServerError, result.error().message());
      }
      return;
    }

    stats_.sim_commands.fetch_add(1);
    stats_.total_commands.fetch_add(1);

    // Build JSON response
    json response;
    response["status"] = "ok";
    response["count"] = result->size();
    response["mode"] = mode;

    json results_array = json::array();
    for (const auto& sim_result : *result) {
      json item;
      item["id"] = sim_result.item_id;
      item["score"] = sim_result.score;
      results_array.push_back(item);
    }
    response["results"] = results_array;

    SendJson(res, kHttpOk, response);

  } catch (const json::parse_error& e) {
    SendError(res, kHttpBadRequest, "Invalid JSON: " + std::string(e.what()));
  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

void HttpServer::HandleSimv(const httplib::Request& req, httplib::Response& res) {
  ;

  // Check if server is loading
  if (loading_ != nullptr && loading_->load()) {
    SendError(res, kHttpServiceUnavailable, "Server is loading, please try again later");
    return;
  }

  try {
    // Parse JSON body
    json body = json::parse(req.body);

    // Validate required fields
    if (!body.contains("vector")) {
      SendError(res, kHttpBadRequest, "Missing required field: vector");
      return;
    }

    std::vector<float> vector = body["vector"].get<std::vector<float>>();
    int top_k = body.value("top_k", handler_context_->config->similarity.default_top_k);

    // Check if similarity engine is initialized
    if (!handler_context_->similarity_engine) {
      SendError(res, kHttpInternalServerError, "Similarity engine not initialized");
      return;
    }

    // Search by vector
    auto result = handler_context_->similarity_engine->SearchByVector(vector, top_k);
    if (!result) {
      SendError(res, kHttpInternalServerError, result.error().message());
      return;
    }

    stats_.total_commands.fetch_add(1);

    // Build JSON response
    json response;
    response["status"] = "ok";
    response["count"] = result->size();
    response["dimension"] = vector.size();

    json results_array = json::array();
    for (const auto& sim_result : *result) {
      json item;
      item["id"] = sim_result.item_id;
      item["score"] = sim_result.score;
      results_array.push_back(item);
    }
    response["results"] = results_array;

    SendJson(res, kHttpOk, response);

  } catch (const json::parse_error& e) {
    SendError(res, kHttpBadRequest, "Invalid JSON: " + std::string(e.what()));
  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

//
// Snapshot management handlers
// Delegate to RequestDispatcher for consistency with TCP protocol
//

void HttpServer::HandleDumpSave(const httplib::Request& req, httplib::Response& res) {
  ;

  try {
    // Parse JSON body (filepath is optional)
    std::string filepath;
    if (!req.body.empty()) {
      json body = json::parse(req.body);
      filepath = body.value("filepath", "");
    }

    // Create Command and use RequestDispatcher
    Command cmd;
    cmd.type = CommandType::kDumpSave;
    cmd.path = filepath;

    RequestDispatcher dispatcher(*handler_context_);
    ConnectionContext conn_ctx;  // Dummy context for HTTP

    auto result_str = dispatcher.Dispatch("DUMP SAVE " + filepath, conn_ctx);

    // Parse result (RequestDispatcher returns formatted text response)
    json response;
    response["status"] = "ok";
    response["message"] = result_str;
    if (!filepath.empty()) {
      response["filepath"] = filepath;
    }
    SendJson(res, kHttpOk, response);

  } catch (const json::parse_error& e) {
    SendError(res, kHttpBadRequest, "Invalid JSON: " + std::string(e.what()));
  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

void HttpServer::HandleDumpLoad(const httplib::Request& req, httplib::Response& res) {
  ;

  try {
    // Parse JSON body
    json body = json::parse(req.body);

    if (!body.contains("filepath")) {
      SendError(res, kHttpBadRequest, "Missing required field: filepath");
      return;
    }

    std::string filepath = body["filepath"];

    // Use RequestDispatcher
    RequestDispatcher dispatcher(*handler_context_);
    ConnectionContext conn_ctx;  // Dummy context for HTTP

    auto result_str = dispatcher.Dispatch("DUMP LOAD " + filepath, conn_ctx);

    // Check if result indicates error
    if (result_str.find("ERROR") != std::string::npos) {
      if (result_str.find("not found") != std::string::npos) {
        SendError(res, kHttpNotFound, result_str);
      } else {
        SendError(res, kHttpInternalServerError, result_str);
      }
      return;
    }

    json response;
    response["status"] = "ok";
    response["message"] = result_str;
    response["filepath"] = filepath;
    SendJson(res, kHttpOk, response);

  } catch (const json::parse_error& e) {
    SendError(res, kHttpBadRequest, "Invalid JSON: " + std::string(e.what()));
  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

void HttpServer::HandleDumpVerify(const httplib::Request& req, httplib::Response& res) {
  ;

  try {
    // Parse JSON body
    json body = json::parse(req.body);

    if (!body.contains("filepath")) {
      SendError(res, kHttpBadRequest, "Missing required field: filepath");
      return;
    }

    std::string filepath = body["filepath"];

    // Use RequestDispatcher
    RequestDispatcher dispatcher(*handler_context_);
    ConnectionContext conn_ctx;  // Dummy context for HTTP

    auto result_str = dispatcher.Dispatch("DUMP VERIFY " + filepath, conn_ctx);

    // Check if result indicates error
    if (result_str.find("ERROR") != std::string::npos) {
      if (result_str.find("not found") != std::string::npos) {
        SendError(res, kHttpNotFound, result_str);
      } else {
        SendError(res, kHttpInternalServerError, result_str);
      }
      return;
    }

    json response;
    response["status"] = "ok";
    response["message"] = result_str;
    response["filepath"] = filepath;
    response["valid"] = true;
    SendJson(res, kHttpOk, response);

  } catch (const json::parse_error& e) {
    SendError(res, kHttpBadRequest, "Invalid JSON: " + std::string(e.what()));
  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

void HttpServer::HandleDumpInfo(const httplib::Request& req, httplib::Response& res) {
  ;

  try {
    // Parse JSON body
    json body = json::parse(req.body);

    if (!body.contains("filepath")) {
      SendError(res, kHttpBadRequest, "Missing required field: filepath");
      return;
    }

    std::string filepath = body["filepath"];

    // Use RequestDispatcher
    RequestDispatcher dispatcher(*handler_context_);
    ConnectionContext conn_ctx;  // Dummy context for HTTP

    auto result_str = dispatcher.Dispatch("DUMP INFO " + filepath, conn_ctx);

    // Check if result indicates error
    if (result_str.find("ERROR") != std::string::npos) {
      if (result_str.find("not found") != std::string::npos) {
        SendError(res, kHttpNotFound, result_str);
      } else {
        SendError(res, kHttpInternalServerError, result_str);
      }
      return;
    }

    json response;
    response["status"] = "ok";
    response["filepath"] = filepath;
    response["info"] = result_str;
    SendJson(res, kHttpOk, response);

  } catch (const json::parse_error& e) {
    SendError(res, kHttpBadRequest, "Invalid JSON: " + std::string(e.what()));
  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

//
// Debug mode handlers
// Note: HTTP debug mode is per-connection concept from TCP, but we provide endpoints for consistency
//

void HttpServer::HandleDebugOn(const httplib::Request& /*req*/, httplib::Response& res) {
  ;

  // Note: HTTP is stateless, so we cannot enable per-connection debug mode
  // This endpoint exists for API compatibility, but has limited functionality
  json response;
  response["status"] = "ok";
  response["message"] = "Debug mode enabled (note: HTTP is stateless, use TCP for per-connection debug)";
  SendJson(res, kHttpOk, response);
}

void HttpServer::HandleDebugOff(const httplib::Request& /*req*/, httplib::Response& res) {
  ;

  // Note: HTTP is stateless, so we cannot disable per-connection debug mode
  json response;
  response["status"] = "ok";
  response["message"] = "Debug mode disabled (note: HTTP is stateless, use TCP for per-connection debug)";
  SendJson(res, kHttpOk, response);
}

//
// Metrics handler (Prometheus-compatible)
// Reference: ../mygram-db/src/server/http_server.cpp:HandleMetrics()
// Reusability: 70% (adapted for nvecd metrics)
//

void HttpServer::HandleMetrics(const httplib::Request& /*req*/, httplib::Response& res) {
  ;

  try {
    // Use TCP server's stats if available (includes all protocol stats), otherwise use HTTP-only stats
    const ServerStats& effective_stats = (tcp_stats_ != nullptr) ? *tcp_stats_ : stats_;

    std::ostringstream metrics;

    // Server uptime
    metrics << "# HELP nvecd_uptime_seconds Server uptime in seconds\n";
    metrics << "# TYPE nvecd_uptime_seconds counter\n";
    metrics << "nvecd_uptime_seconds " << 0 << "\n\n";  // Simple ServerStats doesn't track uptime

    // Total commands
    metrics << "# HELP nvecd_commands_total Total commands processed\n";
    metrics << "# TYPE nvecd_commands_total counter\n";
    metrics << "nvecd_commands_total{command=\"event\"} " << effective_stats.event_commands.load() << "\n";
    metrics << "nvecd_commands_total{command=\"vecset\"} " << effective_stats.vecset_commands.load() << "\n";
    metrics << "nvecd_commands_total{command=\"sim\"} " << effective_stats.sim_commands.load() << "\n";
    metrics << "nvecd_commands_total " << effective_stats.total_commands.load() << "\n\n";

    // Memory usage
    size_t total_memory = 0;
    if (handler_context_ != nullptr && handler_context_->event_store) {
      total_memory += handler_context_->event_store->MemoryUsage();
    }
    if (handler_context_ != nullptr && handler_context_->vector_store) {
      total_memory += handler_context_->vector_store->MemoryUsage();
    }
    if (handler_context_ != nullptr && handler_context_->co_index) {
      total_memory += handler_context_->co_index->MemoryUsage();
    }

    metrics << "# HELP nvecd_memory_bytes Current memory usage in bytes\n";
    metrics << "# TYPE nvecd_memory_bytes gauge\n";
    metrics << "nvecd_memory_bytes " << total_memory << "\n\n";

    // Vector count
    if (handler_context_ != nullptr && handler_context_->vector_store) {
      auto vector_stats = handler_context_->vector_store->GetStatistics();
      metrics << "# HELP nvecd_vectors_total Total vectors stored\n";
      metrics << "# TYPE nvecd_vectors_total gauge\n";
      metrics << "nvecd_vectors_total " << vector_stats.vector_count << "\n\n";
    }

    // Event count
    if (handler_context_ != nullptr && handler_context_->event_store) {
      auto event_stats = handler_context_->event_store->GetStatistics();
      metrics << "# HELP nvecd_events_total Total events stored\n";
      metrics << "# TYPE nvecd_events_total gauge\n";
      metrics << "nvecd_events_total " << event_stats.total_events << "\n\n";

      metrics << "# HELP nvecd_contexts_total Total contexts stored\n";
      metrics << "# TYPE nvecd_contexts_total gauge\n";
      metrics << "nvecd_contexts_total " << event_stats.active_contexts << "\n\n";
    }

    // Cache metrics
    if (handler_context_ != nullptr && handler_context_->cache) {
      auto cache_stats = handler_context_->cache->GetStatistics();

      metrics << "# HELP nvecd_cache_queries_total Total cache queries\n";
      metrics << "# TYPE nvecd_cache_queries_total counter\n";
      metrics << "nvecd_cache_queries_total " << cache_stats.total_queries << "\n\n";

      metrics << "# HELP nvecd_cache_hits_total Total cache hits\n";
      metrics << "# TYPE nvecd_cache_hits_total counter\n";
      metrics << "nvecd_cache_hits_total " << cache_stats.cache_hits << "\n\n";

      metrics << "# HELP nvecd_cache_misses_total Total cache misses\n";
      metrics << "# TYPE nvecd_cache_misses_total counter\n";
      metrics << "nvecd_cache_misses_total " << cache_stats.cache_misses << "\n\n";

      metrics << "# HELP nvecd_cache_hit_rate Cache hit rate\n";
      metrics << "# TYPE nvecd_cache_hit_rate gauge\n";
      metrics << "nvecd_cache_hit_rate " << cache_stats.HitRate() << "\n\n";

      metrics << "# HELP nvecd_cache_entries Current cache entries\n";
      metrics << "# TYPE nvecd_cache_entries gauge\n";
      metrics << "nvecd_cache_entries " << cache_stats.current_entries << "\n\n";

      metrics << "# HELP nvecd_cache_memory_bytes Current cache memory usage\n";
      metrics << "# TYPE nvecd_cache_memory_bytes gauge\n";
      metrics << "nvecd_cache_memory_bytes " << cache_stats.current_memory_bytes << "\n\n";
    }

    res.status = kHttpOk;
    res.set_content(metrics.str(), "text/plain; version=0.0.4; charset=utf-8");

  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

//
// Cache management handlers
//

void HttpServer::HandleCacheStats(const httplib::Request& /*req*/, httplib::Response& res) {
  ;

  try {
    if (handler_context_ == nullptr || handler_context_->cache == nullptr) {
      SendError(res, kHttpInternalServerError, "Cache not initialized");
      return;
    }

    auto stats = handler_context_->cache->GetStatistics();

    json response;
    response["enabled"] = true;
    response["total_queries"] = stats.total_queries;
    response["cache_hits"] = stats.cache_hits;
    response["cache_misses"] = stats.cache_misses;
    response["cache_misses_invalidated"] = stats.cache_misses_invalidated;
    response["cache_misses_not_found"] = stats.cache_misses_not_found;
    response["hit_rate"] = stats.HitRate();
    response["current_entries"] = stats.current_entries;
    response["current_memory_bytes"] = stats.current_memory_bytes;
    response["current_memory_mb"] = static_cast<double>(stats.current_memory_bytes) / (1024.0 * 1024.0);
    response["evictions"] = stats.evictions;
    response["avg_hit_latency_ms"] = stats.AverageCacheHitLatency();
    response["avg_miss_latency_ms"] = stats.AverageCacheMissLatency();
    response["time_saved_ms"] = stats.TotalTimeSaved();

    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

void HttpServer::HandleCacheClear(const httplib::Request& req, httplib::Response& res) {
  ;

  try {
    if (handler_context_ == nullptr || handler_context_->cache == nullptr) {
      SendError(res, kHttpInternalServerError, "Cache not initialized");
      return;
    }

    // Parse optional scope parameter
    std::string scope = "all";
    if (!req.body.empty()) {
      try {
        json body = json::parse(req.body);
        scope = body.value("scope", "all");
      } catch (const json::parse_error&) {
        // Ignore parse errors, use default scope
      }
    }

    // Get stats before clearing
    auto stats_before = handler_context_->cache->GetStatistics();
    uint64_t entries_before = stats_before.current_entries;

    // Clear cache (currently only supports clearing all)
    if (scope == "all") {
      handler_context_->cache->Clear();
    } else {
      SendError(res, kHttpBadRequest, "Invalid scope. Only 'all' is supported currently.");
      return;
    }

    json response;
    response["status"] = "ok";
    response["scope"] = scope;
    response["entries_removed"] = entries_before;

    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    SendError(res, kHttpInternalServerError, "Internal error: " + std::string(e.what()));
  }
}

}  // namespace nvecd::server

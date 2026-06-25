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
#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <sstream>
#include <string_view>

#include "cache/cache_key.h"
#include "cache/cache_key_generator.h"
#include "cache/similarity_cache.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/command_parser.h"
#include "server/filter_parser.h"
#include "server/handlers/dump_handler.h"
#include "similarity/similarity_engine.h"
#include "storage/snapshot_fork.h"
#include "utils/memory_utils.h"
#include "utils/network_utils.h"
#include "utils/string_utils.h"
#include "utils/structured_log.h"
#include "vectors/metadata_store.h"
#include "vectors/vector_store.h"
#include "version.h"

using json = nlohmann::json;

namespace nvecd::server {

namespace {
// HTTP status codes
constexpr int kHttpOk = 200;
constexpr int kHttpNoContent = 204;
constexpr int kHttpBadRequest = 400;
constexpr int kHttpUnauthorized = 401;
constexpr int kHttpForbidden = 403;
constexpr int kHttpNotFound = 404;
constexpr int kHttpInternalServerError = 500;
constexpr int kHttpServiceUnavailable = 503;

// Server startup delay (milliseconds)
constexpr int kStartupDelayMs = 100;

/**
 * @brief Decode a base64 string.
 * @return Decoded bytes, or nullopt if the input is not valid base64.
 */
std::optional<std::string> Base64Decode(const std::string& input) {
  static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::array<int, 256> lookup{};
  lookup.fill(-1);
  for (size_t i = 0; i < kAlphabet.size(); ++i) {
    lookup[static_cast<unsigned char>(kAlphabet[i])] = static_cast<int>(i);
  }

  std::string out;
  int accum = 0;
  int bits = 0;
  for (char ch : input) {
    if (ch == '=') {
      break;
    }
    int value = lookup[static_cast<unsigned char>(ch)];
    if (value < 0) {
      return std::nullopt;
    }
    accum = (accum << 6) | value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((accum >> bits) & 0xFF));
    }
  }
  return out;
}

/**
 * @brief Map a domain error code to an appropriate HTTP status code.
 *
 * Keeps HTTP status semantics aligned with the typed error returned by the
 * shared handlers, instead of sniffing POSIX error strings.
 */
int ErrorCodeToHttpStatus(utils::ErrorCode code) {
  switch (code) {
    case utils::ErrorCode::kVectorNotFound:
    case utils::ErrorCode::kNotFound:
    case utils::ErrorCode::kStorageFileNotFound:
    case utils::ErrorCode::kConfigFileNotFound:
      return kHttpNotFound;
    case utils::ErrorCode::kInvalidArgument:
    case utils::ErrorCode::kCommandInvalidArgument:
    case utils::ErrorCode::kCommandMissingArgument:
    case utils::ErrorCode::kVectorDimensionMismatch:
    case utils::ErrorCode::kVectorInvalidDimension:
      return kHttpBadRequest;
    case utils::ErrorCode::kPermissionDenied:
      return kHttpForbidden;
    default:
      return kHttpInternalServerError;
  }
}

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

std::vector<std::pair<std::string, float>> ApplyMinScore(const std::vector<similarity::SimilarityResult>& results,
                                                         float min_score) {
  std::vector<std::pair<std::string, float>> filtered;
  filtered.reserve(results.size());
  for (const auto& item : results) {
    if (item.score >= min_score) {
      filtered.emplace_back(item.item_id, item.score);
    }
  }
  return filtered;
}

std::vector<similarity::SimilarityResult> ApplyMetadataFilter(const std::vector<similarity::SimilarityResult>& results,
                                                              vectors::MetadataStore* metadata_store,
                                                              const vectors::MetadataFilter& filter) {
  if (filter.Empty() || metadata_store == nullptr) {
    return results;
  }

  std::vector<similarity::SimilarityResult> filtered;
  filtered.reserve(results.size());
  for (const auto& item : results) {
    if (metadata_store->Matches(item.item_id, filter)) {
      filtered.push_back(item);
    }
  }
  return filtered;
}

bool JsonNumberIsFinite(const json& value) {
  if (!value.is_number()) {
    return false;
  }
  return std::isfinite(value.get<double>());
}

utils::Expected<vectors::Metadata, utils::Error> ParseMetadataJson(const json& value) {
  if (!value.is_object()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Field 'metadata' must be an object"));
  }

  vectors::Metadata metadata;
  for (const auto& [key, item] : value.items()) {
    if (key.empty()) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Metadata keys must not be empty"));
    }
    if (item.is_string()) {
      metadata[key] = item.get<std::string>();
    } else if (item.is_boolean()) {
      metadata[key] = item.get<bool>();
    } else if (item.is_number_integer()) {
      metadata[key] = item.get<int64_t>();
    } else if (item.is_number_float()) {
      double number = item.get<double>();
      if (!std::isfinite(number)) {
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Metadata numeric values must be finite"));
      }
      metadata[key] = number;
    } else {
      return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCommandInvalidArgument,
                                                    "Metadata values must be string, integer, float, or bool"));
    }
  }

  return metadata;
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
  server_->Post("/metaset", [this](const httplib::Request& req, httplib::Response& res) { HandleMetaset(req, res); });
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
  server_->Post("/cache/enable",
                [this](const httplib::Request& req, httplib::Response& res) { HandleCacheEnable(req, res); });
  server_->Post("/cache/disable",
                [this](const httplib::Request& req, httplib::Response& res) { HandleCacheDisable(req, res); });

  // Snapshot management
  server_->Post("/dump/save",
                [this](const httplib::Request& req, httplib::Response& res) { HandleDumpSave(req, res); });
  server_->Post("/dump/load",
                [this](const httplib::Request& req, httplib::Response& res) { HandleDumpLoad(req, res); });
  server_->Post("/dump/verify",
                [this](const httplib::Request& req, httplib::Response& res) { HandleDumpVerify(req, res); });
  server_->Post("/dump/info",
                [this](const httplib::Request& req, httplib::Response& res) { HandleDumpInfo(req, res); });
  server_->Get("/dump/status",
               [this](const httplib::Request& req, httplib::Response& res) { HandleDumpStatus(req, res); });

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

bool HttpServer::IsAuthorized(const httplib::Request& req) const {
  // No password configured: every request is authorized (matches TCP behavior).
  if (config_.requirepass.empty()) {
    return true;
  }

  if (!req.has_header("Authorization")) {
    return false;
  }
  const std::string auth = req.get_header_value("Authorization");

  // "Bearer <password>"
  static constexpr std::string_view kBearerPrefix = "Bearer ";
  if (auth.size() > kBearerPrefix.size() && auth.compare(0, kBearerPrefix.size(), kBearerPrefix) == 0) {
    return auth.substr(kBearerPrefix.size()) == config_.requirepass;
  }

  // "Basic base64(user:password)" - the username is ignored, only the password
  // is compared, mirroring TCP AUTH which validates the password alone.
  static constexpr std::string_view kBasicPrefix = "Basic ";
  if (auth.size() > kBasicPrefix.size() && auth.compare(0, kBasicPrefix.size(), kBasicPrefix) == 0) {
    auto decoded = Base64Decode(auth.substr(kBasicPrefix.size()));
    if (!decoded) {
      return false;
    }
    auto colon = decoded->find(':');
    const std::string password = (colon == std::string::npos) ? *decoded : decoded->substr(colon + 1);
    return password == config_.requirepass;
  }

  return false;
}

//
// Health check handlers (MygramDB-compatible, Kubernetes-ready)
// Reference: ../mygram-db/src/server/http_server.cpp (lines 974-1098)
// Reusability: 95%
//

void HttpServer::HandleHealth(const httplib::Request& /*req*/, httplib::Response& res) {
  json response;
  response["status"] = "ok";
  response["timestamp"] =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  SendJson(res, kHttpOk, response);
}

void HttpServer::HandleHealthLive(const httplib::Request& /*req*/, httplib::Response& res) {
  // Liveness probe: Always return 200 OK if the process is running
  // This is used by orchestrators (Kubernetes, Docker) to detect deadlocks
  json response;
  response["status"] = "alive";
  response["timestamp"] =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  SendJson(res, kHttpOk, response);
}

void HttpServer::HandleHealthReady(const httplib::Request& /*req*/, httplib::Response& res) {
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
  // Detailed health: Return comprehensive component status
  json response;

  // Overall status
  bool is_loading = (loading_ != nullptr && loading_->load());
  response["status"] = is_loading ? "degraded" : "healthy";
  response["timestamp"] =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  const ServerStats& effective_stats = (tcp_stats_ != nullptr) ? *tcp_stats_ : stats_;
  response["uptime_seconds"] = effective_stats.GetUptimeSeconds();

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
    tcp_stats_->info_commands.fetch_add(1);
  } else {
    stats_.info_commands.fetch_add(1);
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    json response;

    // Use TCP server's stats if available (includes all protocol stats), otherwise use HTTP-only stats
    const ServerStats& effective_stats = (tcp_stats_ != nullptr) ? *tcp_stats_ : stats_;

    // Server info
    response["server"] = "nvecd";
    response["version"] = nvecd::Version::String();
    response["uptime_seconds"] = effective_stats.GetUptimeSeconds();

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

    // Process memory information (retrieved early for peak memory fields)
    auto proc_info = utils::GetProcessMemoryInfo();

    json memory_obj;
    memory_obj["used_memory_bytes"] = total_memory;
    memory_obj["used_memory_human"] = utils::FormatBytes(total_memory);
    memory_obj["peak_memory_bytes"] = proc_info ? proc_info->peak_rss_bytes : 0;
    memory_obj["peak_memory_human"] = utils::FormatBytes(proc_info ? proc_info->peak_rss_bytes : 0);
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

    // Process memory details
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

    // Cache statistics (matches the field names used by /cache/stats).
    auto* info_cache =
        (handler_context_ != nullptr) ? handler_context_->cache.load(std::memory_order_acquire) : nullptr;
    json cache_obj;
    if (info_cache != nullptr) {
      auto cache_stats = info_cache->GetStatistics();
      cache_obj["enabled"] = info_cache->IsEnabled();
      cache_obj["total_queries"] = cache_stats.total_queries;
      cache_obj["cache_hits"] = cache_stats.cache_hits;
      cache_obj["cache_misses"] = cache_stats.cache_misses;
      cache_obj["hit_rate"] = cache_stats.HitRate();
      cache_obj["current_entries"] = cache_stats.current_entries;
      cache_obj["current_memory_bytes"] = cache_stats.current_memory_bytes;
      cache_obj["evictions"] = cache_stats.evictions;
      cache_obj["time_saved_ms"] = cache_stats.TotalTimeSaved();
    } else {
      cache_obj["enabled"] = false;
    }
    response["cache"] = cache_obj;

    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleInfo: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

void HttpServer::HandleConfig(const httplib::Request& /*req*/, httplib::Response& res) {
  if (full_config_ == nullptr) {
    SendError(res, kHttpInternalServerError, "Configuration not available");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
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
    spdlog::error("Unexpected exception in HandleConfig: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

//
// nvecd-specific operation handlers
// These reuse the same stores, cache key components, and typed handlers as the
// TCP path so behavior stays consistent across surfaces.
//

void HttpServer::HandleEvent(const httplib::Request& req, httplib::Response& res) {
  if (!IsAuthorized(req)) {
    SendError(res, kHttpUnauthorized, "Authentication required");
    return;
  }

  // Check if server is loading
  if (loading_ != nullptr && loading_->load()) {
    SendError(res, kHttpServiceUnavailable, "Server is loading, please try again later");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    // Parse JSON body (non-throwing)
    auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
      SendError(res, kHttpBadRequest, "Invalid JSON body");
      return;
    }

    // Validate required fields
    if (!body.contains("ctx") || !body.contains("id") || !body.contains("type")) {
      SendError(res, kHttpBadRequest, "Missing required fields: ctx, id, type");
      return;
    }

    if (!body["ctx"].is_string() || !body["id"].is_string() || !body["type"].is_string()) {
      SendError(res, kHttpBadRequest, "Fields ctx, id, type must be strings");
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
      if (!JsonNumberIsFinite(body["score"])) {
        SendError(res, kHttpBadRequest, "Field 'score' must be a number");
        return;
      }
      score = body["score"];
    }

    uint64_t timestamp = 0;
    if (body.contains("timestamp")) {
      if (!body["timestamp"].is_number_unsigned()) {
        SendError(res, kHttpBadRequest, "Field 'timestamp' must be an unsigned integer");
        return;
      }
      timestamp = body["timestamp"];
    }

    // Add event to event store, atomically capturing the prior buffer state.
    if (handler_context_->event_store == nullptr) {
      SendError(res, kHttpInternalServerError, "Event store not initialized");
      return;
    }
    auto result = handler_context_->event_store->AddEventAndGetPrior(ctx, id, score, event_type, timestamp);
    if (!result) {
      SendError(res, kHttpInternalServerError, result.error().message());
      return;
    }

    // Update co-occurrence index incrementally (only new pairs, once each).
    if (handler_context_->co_index != nullptr && !result->deduped) {
      events::CoOccurrenceIndex::IngestOptions options;
      if (handler_context_->config != nullptr) {
        options.temporal_enabled = handler_context_->config->events.temporal_cooccurrence;
        options.half_life_sec = handler_context_->config->events.temporal_half_life_sec;
        options.negative_signals = handler_context_->config->events.negative_signals;
        options.negative_weight = handler_context_->config->events.negative_weight;
      }
      handler_context_->co_index->ApplyIngestedEvent(ctx, result->prior_events, result->stored_event, options);
    }

    auto* cache_ptr = handler_context_->cache.load(std::memory_order_acquire);
    if (cache_ptr != nullptr) {
      cache_ptr->InvalidateByItemId(id);
    }

    handler_context_->stats.event_commands.fetch_add(1);
    handler_context_->stats.total_commands.fetch_add(1);

    json response;
    response["status"] = "ok";
    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleEvent: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

void HttpServer::HandleVecset(const httplib::Request& req, httplib::Response& res) {
  if (!IsAuthorized(req)) {
    SendError(res, kHttpUnauthorized, "Authentication required");
    return;
  }

  // Check if server is loading
  if (loading_ != nullptr && loading_->load()) {
    SendError(res, kHttpServiceUnavailable, "Server is loading, please try again later");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    // Parse JSON body (non-throwing)
    auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
      SendError(res, kHttpBadRequest, "Invalid JSON body");
      return;
    }

    // Validate required fields
    if (!body.contains("id") || !body.contains("vector")) {
      SendError(res, kHttpBadRequest, "Missing required fields: id, vector");
      return;
    }

    if (!body["id"].is_string()) {
      SendError(res, kHttpBadRequest, "Field 'id' must be a string");
      return;
    }

    if (!body["vector"].is_array()) {
      SendError(res, kHttpBadRequest, "Field 'vector' must be an array of numbers");
      return;
    }

    // Validate all vector elements are finite numbers
    for (const auto& elem : body["vector"]) {
      if (!JsonNumberIsFinite(elem)) {
        SendError(res, kHttpBadRequest, "Field 'vector' must contain only finite numbers");
        return;
      }
    }

    std::string id = body["id"];
    std::vector<float> vector = body["vector"].get<std::vector<float>>();
    if (vector.empty()) {
      SendError(res, kHttpBadRequest, "Field 'vector' must not be empty");
      return;
    }

    vectors::Metadata metadata;
    bool has_metadata = body.contains("metadata");
    if (has_metadata) {
      auto metadata_result = ParseMetadataJson(body["metadata"]);
      if (!metadata_result) {
        SendError(res, kHttpBadRequest, metadata_result.error().message());
        return;
      }
      metadata = std::move(*metadata_result);
    }

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

    auto compact_idx = handler_context_->vector_store->GetCompactIndex(id);
    if (compact_idx.has_value()) {
      if (has_metadata && handler_context_->metadata_store != nullptr) {
        handler_context_->metadata_store->Set(id, std::move(metadata));
      }

      if (handler_context_->similarity_engine != nullptr) {
        handler_context_->similarity_engine->NotifyVectorAdded(compact_idx.value(), vector.data());
      }
    }

    auto* cache_ptr = handler_context_->cache.load(std::memory_order_acquire);
    if (cache_ptr != nullptr) {
      cache_ptr->InvalidateByItemId(id);
      if (has_metadata) {
        cache_ptr->Clear();
      }
    }

    handler_context_->stats.vecset_commands.fetch_add(1);
    handler_context_->stats.total_commands.fetch_add(1);

    json response;
    response["status"] = "ok";
    response["dimension"] = vector.size();
    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleVecset: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

void HttpServer::HandleMetaset(const httplib::Request& req, httplib::Response& res) {
  if (!IsAuthorized(req)) {
    SendError(res, kHttpUnauthorized, "Authentication required");
    return;
  }

  // Check if server is loading
  if (loading_ != nullptr && loading_->load()) {
    SendError(res, kHttpServiceUnavailable, "Server is loading, please try again later");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    // Parse JSON body (non-throwing)
    auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
      SendError(res, kHttpBadRequest, "Invalid JSON body");
      return;
    }

    // Validate required fields
    if (!body.contains("id") || !body.contains("metadata")) {
      SendError(res, kHttpBadRequest, "Missing required fields: id, metadata");
      return;
    }
    if (!body["id"].is_string()) {
      SendError(res, kHttpBadRequest, "Field 'id' must be a string");
      return;
    }

    std::string id = body["id"];

    auto metadata_result = ParseMetadataJson(body["metadata"]);
    if (!metadata_result) {
      SendError(res, kHttpBadRequest, metadata_result.error().message());
      return;
    }

    if (handler_context_->vector_store == nullptr) {
      SendError(res, kHttpInternalServerError, "Vector store not initialized");
      return;
    }
    if (handler_context_->metadata_store == nullptr) {
      SendError(res, kHttpInternalServerError, "Metadata store not initialized");
      return;
    }

    // Metadata is keyed by item id; the target vector must already exist
    // (mirrors the TCP METASET behavior in RequestDispatcher).
    if (!handler_context_->vector_store->GetCompactIndex(id).has_value()) {
      SendError(res, kHttpNotFound, "Vector not found for metadata: " + id);
      return;
    }

    handler_context_->metadata_store->Set(id, std::move(*metadata_result));

    // Metadata changes affect filtered results broadly: clear the cache, as TCP does.
    auto* cache_ptr = handler_context_->cache.load(std::memory_order_acquire);
    if (cache_ptr != nullptr) {
      cache_ptr->Clear();
    }

    handler_context_->stats.total_commands.fetch_add(1);

    json response;
    response["status"] = "ok";
    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleMetaset: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

void HttpServer::HandleSim(const httplib::Request& req, httplib::Response& res) {
  // Check if server is loading
  if (loading_ != nullptr && loading_->load()) {
    SendError(res, kHttpServiceUnavailable, "Server is loading, please try again later");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    // Parse JSON body (non-throwing)
    auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
      SendError(res, kHttpBadRequest, "Invalid JSON body");
      return;
    }

    // Validate required fields
    if (!body.contains("id")) {
      SendError(res, kHttpBadRequest, "Missing required field: id");
      return;
    }

    if (!body["id"].is_string()) {
      SendError(res, kHttpBadRequest, "Field 'id' must be a string");
      return;
    }

    std::string id = body["id"];
    int top_k = body.value("top_k", handler_context_->config->similarity.default_top_k);
    std::string mode = body.value("mode", "fusion");
    float min_score = body.value("min_score", 0.0F);
    if (!std::isfinite(min_score)) {
      SendError(res, kHttpBadRequest, "Field 'min_score' must be finite");
      return;
    }

    std::string filter_expr;
    vectors::MetadataFilter filter;
    if (body.contains("filter")) {
      if (!body["filter"].is_string()) {
        SendError(res, kHttpBadRequest, "Field 'filter' must be a string");
        return;
      }
      filter_expr = body["filter"].get<std::string>();
      auto filter_result = ParseSimpleFilter(filter_expr);
      if (!filter_result) {
        SendError(res, kHttpBadRequest, filter_result.error().message());
        return;
      }
      filter = std::move(*filter_result);
    }

    std::optional<bool> adaptive;
    if (body.contains("adaptive")) {
      if (!body["adaptive"].is_boolean()) {
        SendError(res, kHttpBadRequest, "Field 'adaptive' must be a boolean");
        return;
      }
      adaptive = body["adaptive"].get<bool>();
    }

    if (mode != "events" && mode != "vectors" && mode != "fusion") {
      SendError(res, kHttpBadRequest, "Invalid mode. Must be one of: events, vectors, fusion");
      return;
    }

    // Check if similarity engine is initialized
    if (!handler_context_->similarity_engine) {
      SendError(res, kHttpInternalServerError, "Similarity engine not initialized");
      return;
    }

    // Cache lookup (same key components and search type as the TCP path).
    auto* cache_ptr = handler_context_->cache.load(std::memory_order_acquire);
    const bool cache_enabled = (cache_ptr != nullptr && cache_ptr->IsEnabled());
    auto search_type = filter_expr.empty() ? cache::SearchType::kItemSearch : cache::SearchType::kFilteredSearch;
    cache::CacheKey cache_key;

    if (cache_enabled) {
      const std::string adaptive_part = adaptive.has_value() ? (*adaptive ? "on" : "off") : "default";
      auto generation = handler_context_->co_index != nullptr ? handler_context_->co_index->GetGeneration() : 0;
      std::string key_str = "SIM:" + id + ":" + std::to_string(top_k) + ":" + mode + ":a" + adaptive_part + ":g" +
                            std::to_string(generation);
      if (!filter_expr.empty()) {
        key_str += ":f" + filter_expr;
      }
      cache_key = cache::CacheKeyGenerator::Generate(key_str);

      auto cached = cache_ptr->Lookup(cache_key, search_type);
      if (cached.has_value()) {
        handler_context_->stats.sim_commands.fetch_add(1);
        handler_context_->stats.total_commands.fetch_add(1);

        json response;
        response["status"] = "ok";
        auto cached_results = ApplyMinScore(*cached, min_score);
        response["count"] = cached_results.size();
        response["mode"] = mode;
        json results_array = json::array();
        for (const auto& sim_result : cached_results) {
          json item;
          item["id"] = sim_result.first;
          item["score"] = sim_result.second;
          results_array.push_back(item);
        }
        response["results"] = results_array;
        SendJson(res, kHttpOk, response);
        return;
      }
    }

    auto start = std::chrono::steady_clock::now();

    // Call appropriate search method based on mode
    utils::Expected<std::vector<similarity::SimilarityResult>, utils::Error> result;
    if (mode == "events") {
      result = handler_context_->similarity_engine->SearchByIdEvents(id, top_k);
    } else if (mode == "vectors") {
      result = handler_context_->similarity_engine->SearchByIdVectors(id, top_k, filter);
    } else {  // fusion
      result = handler_context_->similarity_engine->SearchByIdFusion(id, top_k, adaptive, filter);
    }

    if (!result) {
      if (result.error().code() == utils::ErrorCode::kVectorNotFound) {
        SendError(res, kHttpNotFound, result.error().message());
      } else {
        SendError(res, kHttpInternalServerError, result.error().message());
      }
      return;
    }
    *result = ApplyMetadataFilter(*result, handler_context_->metadata_store, filter);

    // Cache store (mirrors the TCP path: cache full results, register items).
    if (cache_enabled) {
      auto elapsed = std::chrono::steady_clock::now() - start;
      double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
      cache_ptr->Insert(cache_key, *result, elapsed_ms, search_type);

      std::vector<std::string> item_ids;
      item_ids.reserve(result->size() + 1);
      item_ids.push_back(id);
      for (const auto& item : *result) {
        item_ids.push_back(item.item_id);
      }
      cache_ptr->RegisterResultItems(cache_key, item_ids);
    }

    handler_context_->stats.sim_commands.fetch_add(1);
    handler_context_->stats.total_commands.fetch_add(1);

    // Build JSON response
    json response;
    response["status"] = "ok";
    auto filtered_results = ApplyMinScore(*result, min_score);
    response["count"] = filtered_results.size();
    response["mode"] = mode;

    json results_array = json::array();
    for (const auto& sim_result : filtered_results) {
      json item;
      item["id"] = sim_result.first;
      item["score"] = sim_result.second;
      results_array.push_back(item);
    }
    response["results"] = results_array;

    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleSim: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

void HttpServer::HandleSimv(const httplib::Request& req, httplib::Response& res) {
  // Check if server is loading
  if (loading_ != nullptr && loading_->load()) {
    SendError(res, kHttpServiceUnavailable, "Server is loading, please try again later");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    // Parse JSON body (non-throwing)
    auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
      SendError(res, kHttpBadRequest, "Invalid JSON body");
      return;
    }

    // Validate required fields
    if (!body.contains("vector")) {
      SendError(res, kHttpBadRequest, "Missing required field: vector");
      return;
    }

    if (!body["vector"].is_array()) {
      SendError(res, kHttpBadRequest, "Field 'vector' must be an array of numbers");
      return;
    }

    // Validate all vector elements are finite numbers
    for (const auto& elem : body["vector"]) {
      if (!JsonNumberIsFinite(elem)) {
        SendError(res, kHttpBadRequest, "Field 'vector' must contain only finite numbers");
        return;
      }
    }

    std::vector<float> vector = body["vector"].get<std::vector<float>>();
    int top_k = body.value("top_k", handler_context_->config->similarity.default_top_k);
    float min_score = body.value("min_score", 0.0F);
    if (!std::isfinite(min_score)) {
      SendError(res, kHttpBadRequest, "Field 'min_score' must be finite");
      return;
    }

    std::string filter_expr;
    vectors::MetadataFilter filter;
    if (body.contains("filter")) {
      if (!body["filter"].is_string()) {
        SendError(res, kHttpBadRequest, "Field 'filter' must be a string");
        return;
      }
      filter_expr = body["filter"].get<std::string>();
      auto filter_result = ParseSimpleFilter(filter_expr);
      if (!filter_result) {
        SendError(res, kHttpBadRequest, filter_result.error().message());
        return;
      }
      filter = std::move(*filter_result);
    }

    // Check if similarity engine is initialized
    if (!handler_context_->similarity_engine) {
      SendError(res, kHttpInternalServerError, "Similarity engine not initialized");
      return;
    }

    // Cache lookup (same key components and search type as the TCP path).
    auto* cache_ptr = handler_context_->cache.load(std::memory_order_acquire);
    const bool cache_enabled = (cache_ptr != nullptr && cache_ptr->IsEnabled());
    auto search_type = filter_expr.empty() ? cache::SearchType::kVectorSearch : cache::SearchType::kFilteredSearch;
    cache::CacheKey cache_key;

    if (cache_enabled) {
      std::string key_str = "SIMV:" + cache::HashVector(vector) + ":" + std::to_string(top_k);
      if (!filter_expr.empty()) {
        key_str += ":f" + filter_expr;
      }
      cache_key = cache::CacheKeyGenerator::Generate(key_str);

      auto cached = cache_ptr->Lookup(cache_key, search_type);
      if (cached.has_value()) {
        handler_context_->stats.sim_commands.fetch_add(1);
        handler_context_->stats.total_commands.fetch_add(1);

        json response;
        response["status"] = "ok";
        auto cached_results = ApplyMinScore(*cached, min_score);
        response["count"] = cached_results.size();
        response["dimension"] = vector.size();
        json results_array = json::array();
        for (const auto& sim_result : cached_results) {
          json item;
          item["id"] = sim_result.first;
          item["score"] = sim_result.second;
          results_array.push_back(item);
        }
        response["results"] = results_array;
        SendJson(res, kHttpOk, response);
        return;
      }
    }

    auto start = std::chrono::steady_clock::now();

    // Search by vector
    auto result = handler_context_->similarity_engine->SearchByVector(vector, top_k, filter);
    if (!result) {
      SendError(res, kHttpInternalServerError, result.error().message());
      return;
    }
    *result = ApplyMetadataFilter(*result, handler_context_->metadata_store, filter);

    // Cache store (mirrors the TCP path).
    if (cache_enabled) {
      auto elapsed = std::chrono::steady_clock::now() - start;
      double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
      cache_ptr->Insert(cache_key, *result, elapsed_ms, search_type);

      std::vector<std::string> item_ids;
      item_ids.reserve(result->size());
      for (const auto& item : *result) {
        item_ids.push_back(item.item_id);
      }
      cache_ptr->RegisterResultItems(cache_key, item_ids);
    }

    handler_context_->stats.sim_commands.fetch_add(1);
    handler_context_->stats.total_commands.fetch_add(1);

    // Build JSON response
    json response;
    response["status"] = "ok";
    auto filtered_results = ApplyMinScore(*result, min_score);
    response["count"] = filtered_results.size();
    response["dimension"] = vector.size();

    json results_array = json::array();
    for (const auto& sim_result : filtered_results) {
      json item;
      item["id"] = sim_result.first;
      item["score"] = sim_result.second;
      results_array.push_back(item);
    }
    response["results"] = results_array;

    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleSimv: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

//
// Snapshot management handlers
// Delegate to the same typed handlers used by the TCP path. Responses are
// built from the structured Expected<> result so that the HTTP status reflects
// the real outcome (no string-sniffing, no hardcoded success flags).
//

namespace {
/// Extract the resolved path token that the dump handlers append after their
/// status keyword (e.g. "OK DUMP_SAVED <path>\r\n").
std::string ExtractDumpPath(const std::string& response, const std::string& fallback) {
  std::istringstream iss(response);
  std::string keyword;
  std::string verb;
  std::string path;
  // Response shape: "OK <VERB> <path>"
  if ((iss >> keyword >> verb >> path) && keyword == "OK" && !path.empty()) {
    return path;
  }
  return fallback;
}
}  // namespace

void HttpServer::HandleDumpSave(const httplib::Request& req, httplib::Response& res) {
  if (!IsAuthorized(req)) {
    SendError(res, kHttpUnauthorized, "Authentication required");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    // Parse JSON body (filepath is optional)
    std::string filepath;
    if (!req.body.empty()) {
      auto body = json::parse(req.body, nullptr, false);
      if (body.is_discarded()) {
        SendError(res, kHttpBadRequest, "Invalid JSON body");
        return;
      }
      filepath = body.value("filepath", "");
    }

    auto result = handlers::HandleDumpSave(*handler_context_, filepath);
    if (!result) {
      SendError(res, ErrorCodeToHttpStatus(result.error().code()), result.error().message());
      return;
    }

    handler_context_->stats.dump_commands.fetch_add(1);
    handler_context_->stats.total_commands.fetch_add(1);

    json response;
    response["status"] = "ok";
    response["filepath"] = ExtractDumpPath(*result, filepath);
    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleDumpSave: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

void HttpServer::HandleDumpLoad(const httplib::Request& req, httplib::Response& res) {
  if (!IsAuthorized(req)) {
    SendError(res, kHttpUnauthorized, "Authentication required");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    // Parse JSON body (non-throwing)
    auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
      SendError(res, kHttpBadRequest, "Invalid JSON body");
      return;
    }

    if (!body.contains("filepath")) {
      SendError(res, kHttpBadRequest, "Missing required field: filepath");
      return;
    }

    if (!body["filepath"].is_string()) {
      SendError(res, kHttpBadRequest, "Field 'filepath' must be a string");
      return;
    }

    std::string filepath = body["filepath"];

    auto result = handlers::HandleDumpLoad(*handler_context_, filepath);
    if (!result) {
      SendError(res, ErrorCodeToHttpStatus(result.error().code()), result.error().message());
      return;
    }

    handler_context_->stats.dump_commands.fetch_add(1);
    handler_context_->stats.total_commands.fetch_add(1);

    json response;
    response["status"] = "ok";
    response["filepath"] = ExtractDumpPath(*result, filepath);
    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleDumpLoad: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

void HttpServer::HandleDumpVerify(const httplib::Request& req, httplib::Response& res) {
  if (!IsAuthorized(req)) {
    SendError(res, kHttpUnauthorized, "Authentication required");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    // Parse JSON body (non-throwing)
    auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
      SendError(res, kHttpBadRequest, "Invalid JSON body");
      return;
    }

    if (!body.contains("filepath")) {
      SendError(res, kHttpBadRequest, "Missing required field: filepath");
      return;
    }

    if (!body["filepath"].is_string()) {
      SendError(res, kHttpBadRequest, "Field 'filepath' must be a string");
      return;
    }

    std::string filepath = body["filepath"];

    auto result = handlers::HandleDumpVerify(handler_context_->dump_dir, filepath);
    if (!result) {
      // Verification failed: report a concrete, non-success response.
      json response;
      response["status"] = "error";
      response["filepath"] = filepath;
      response["valid"] = false;
      response["error"] = result.error().message();
      SendJson(res, ErrorCodeToHttpStatus(result.error().code()), response);
      return;
    }

    json response;
    response["status"] = "ok";
    response["filepath"] = ExtractDumpPath(*result, filepath);
    response["valid"] = true;
    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleDumpVerify: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

void HttpServer::HandleDumpInfo(const httplib::Request& req, httplib::Response& res) {
  if (!IsAuthorized(req)) {
    SendError(res, kHttpUnauthorized, "Authentication required");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    // Parse JSON body (non-throwing)
    auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
      SendError(res, kHttpBadRequest, "Invalid JSON body");
      return;
    }

    if (!body.contains("filepath")) {
      SendError(res, kHttpBadRequest, "Missing required field: filepath");
      return;
    }

    if (!body["filepath"].is_string()) {
      SendError(res, kHttpBadRequest, "Field 'filepath' must be a string");
      return;
    }

    std::string filepath = body["filepath"];

    auto result = handlers::HandleDumpInfo(handler_context_->dump_dir, filepath);
    if (!result) {
      SendError(res, ErrorCodeToHttpStatus(result.error().code()), result.error().message());
      return;
    }

    // The structured handler returns a "key: value" block; surface the parsed
    // fields as JSON so callers get a typed shape rather than a raw text blob.
    json info = json::object();
    std::istringstream iss(*result);
    std::string line;
    while (std::getline(iss, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      auto sep = line.find(": ");
      if (sep == std::string::npos) {
        continue;
      }
      info[line.substr(0, sep)] = line.substr(sep + 2);
    }

    json response;
    response["status"] = "ok";
    response["filepath"] = filepath;
    response["info"] = info;
    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleDumpInfo: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

//
// Debug mode handlers
// Note: HTTP debug mode is per-connection concept from TCP, but we provide endpoints for consistency
//

void HttpServer::HandleDebugOn(const httplib::Request& /*req*/, httplib::Response& res) {
  // Note: HTTP is stateless, so we cannot enable per-connection debug mode
  // This endpoint exists for API compatibility, but has limited functionality
  json response;
  response["status"] = "ok";
  response["message"] = "Debug mode enabled (note: HTTP is stateless, use TCP for per-connection debug)";
  SendJson(res, kHttpOk, response);
}

void HttpServer::HandleDebugOff(const httplib::Request& /*req*/, httplib::Response& res) {
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
  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    // Use TCP server's stats if available (includes all protocol stats), otherwise use HTTP-only stats
    const ServerStats& effective_stats = (tcp_stats_ != nullptr) ? *tcp_stats_ : stats_;

    std::ostringstream metrics;

    // Server uptime
    metrics << "# HELP nvecd_uptime_seconds Server uptime in seconds\n";
    metrics << "# TYPE nvecd_uptime_seconds counter\n";
    metrics << "nvecd_uptime_seconds " << effective_stats.GetUptimeSeconds() << "\n\n";

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
    auto* metrics_cache =
        (handler_context_ != nullptr) ? handler_context_->cache.load(std::memory_order_acquire) : nullptr;
    if (metrics_cache != nullptr) {
      auto cache_stats = metrics_cache->GetStatistics();

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
    spdlog::error("Unexpected exception in HandleMetrics: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

//
// Cache management handlers
//

void HttpServer::HandleCacheStats(const httplib::Request& /*req*/, httplib::Response& res) {
  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    auto* stats_cache =
        (handler_context_ != nullptr) ? handler_context_->cache.load(std::memory_order_acquire) : nullptr;
    if (stats_cache == nullptr) {
      SendError(res, kHttpInternalServerError, "Cache not initialized");
      return;
    }

    auto stats = stats_cache->GetStatistics();

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
    spdlog::error("Unexpected exception in HandleCacheStats: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

void HttpServer::HandleCacheClear(const httplib::Request& req, httplib::Response& res) {
  if (!IsAuthorized(req)) {
    SendError(res, kHttpUnauthorized, "Authentication required");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    auto* clear_cache =
        (handler_context_ != nullptr) ? handler_context_->cache.load(std::memory_order_acquire) : nullptr;
    if (clear_cache == nullptr) {
      SendError(res, kHttpInternalServerError, "Cache not initialized");
      return;
    }

    // Parse optional scope parameter (non-throwing)
    std::string scope = "all";
    if (!req.body.empty()) {
      auto body = json::parse(req.body, nullptr, false);
      if (!body.is_discarded()) {
        scope = body.value("scope", "all");
      }
      // Ignore parse errors, use default scope
    }

    // Get stats before clearing
    auto stats_before = clear_cache->GetStatistics();
    uint64_t entries_before = stats_before.current_entries;

    // Clear cache (currently only supports clearing all)
    if (scope == "all") {
      clear_cache->Clear();
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
    spdlog::error("Unexpected exception in HandleCacheClear: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

void HttpServer::HandleCacheEnable(const httplib::Request& req, httplib::Response& res) {
  if (!IsAuthorized(req)) {
    SendError(res, kHttpUnauthorized, "Authentication required");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    auto* cache_ptr = (handler_context_ != nullptr) ? handler_context_->cache.load(std::memory_order_acquire) : nullptr;
    if (cache_ptr == nullptr) {
      SendError(res, kHttpInternalServerError, "Cache not initialized");
      return;
    }

    cache_ptr->SetEnabled(true);

    json response;
    response["status"] = "ok";
    response["message"] = "Cache enabled";
    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleCacheEnable: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

void HttpServer::HandleCacheDisable(const httplib::Request& req, httplib::Response& res) {
  if (!IsAuthorized(req)) {
    SendError(res, kHttpUnauthorized, "Authentication required");
    return;
  }

  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    auto* cache_ptr = (handler_context_ != nullptr) ? handler_context_->cache.load(std::memory_order_acquire) : nullptr;
    if (cache_ptr == nullptr) {
      SendError(res, kHttpInternalServerError, "Cache not initialized");
      return;
    }

    cache_ptr->SetEnabled(false);

    json response;
    response["status"] = "ok";
    response["message"] = "Cache disabled";
    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleCacheDisable: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

void HttpServer::HandleDumpStatus(const httplib::Request& /*req*/, httplib::Response& res) {
  // Safety net at httplib library boundary - catches unexpected exceptions only
  try {
    if (handler_context_ == nullptr || handler_context_->fork_snapshot_writer == nullptr) {
      json response;
      response["status"] = "ok";
      response["data"] = "IDLE";
      SendJson(res, kHttpOk, response);
      return;
    }

    // Reap any finished child
    handler_context_->fork_snapshot_writer->CheckChild();

    auto snapshot_status = handler_context_->fork_snapshot_writer->GetStatus();

    json response;
    response["status"] = "ok";

    switch (snapshot_status.status) {
      case storage::SnapshotStatus::kIdle:
        response["data"] = "IDLE";
        break;
      case storage::SnapshotStatus::kInProgress:
        response["data"] = "IN_PROGRESS";
        response["filepath"] = snapshot_status.filepath;
        break;
      case storage::SnapshotStatus::kCompleted:
        response["data"] = "COMPLETED";
        response["filepath"] = snapshot_status.filepath;
        break;
      case storage::SnapshotStatus::kFailed:
        response["data"] = "FAILED";
        response["filepath"] = snapshot_status.filepath;
        response["error_message"] = snapshot_status.error_message;
        break;
    }

    SendJson(res, kHttpOk, response);

  } catch (const std::exception& e) {
    spdlog::error("Unexpected exception in HandleDumpStatus: {}", e.what());
    SendError(res, kHttpInternalServerError, R"({"error":"Internal server error"})");
  }
}

}  // namespace nvecd::server

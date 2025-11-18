/**
 * @file nvecd_server.cpp
 * @brief Implementation of NvecdServer
 *
 * Reference: ../mygram-db/src/server/tcp_server.cpp
 * Reusability: 65% (similar patterns, simplified for nvecd)
 */

#include "server/nvecd_server.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include "server/connection_io_handler.h"
#include "utils/error.h"
#include "utils/structured_log.h"

namespace nvecd::server {

namespace {
constexpr int kShutdownTimeoutMs = 5000;  // Wait up to 5s for connections to close
constexpr int kShutdownCheckIntervalMs = 100;
}  // namespace

NvecdServer::NvecdServer(const config::Config& config) : config_(config) {}

NvecdServer::~NvecdServer() {
  if (running_.load()) {
    Stop();
  }
}

utils::Expected<void, utils::Error> NvecdServer::Start() {
  if (running_.load()) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kAlreadyExists, "Server already running"));
  }

  spdlog::info("Starting nvecd server...");

  // Initialize components
  auto init_result = InitializeComponents();
  if (!init_result) {
    return init_result;
  }

  // Create thread pool
  int worker_threads = config_.perf.thread_pool_size;
  if (worker_threads <= 0) {
    worker_threads = static_cast<int>(std::thread::hardware_concurrency());
  }
  thread_pool_ = std::make_unique<ThreadPool>(static_cast<size_t>(worker_threads));

  spdlog::info("Thread pool created with {} workers", worker_threads);

  // Create ServerConfig for connection acceptor
  ServerConfig server_config;
  server_config.host = config_.api.tcp.bind;
  server_config.port = static_cast<uint16_t>(config_.api.tcp.port);
  server_config.max_connections = config_.perf.max_connections;
  server_config.worker_threads = worker_threads;

  // Parse allowed CIDRs
  server_config.allow_cidrs = config_.network.allow_cidrs;
  for (const auto& cidr_str : config_.network.allow_cidrs) {
    auto cidr = utils::CIDR::Parse(cidr_str);
    if (cidr) {
      server_config.parsed_allow_cidrs.push_back(*cidr);
    } else {
      spdlog::warn("Invalid CIDR in configuration: {}", cidr_str);
    }
  }

  // Create connection acceptor
  acceptor_ = std::make_unique<ConnectionAcceptor>(server_config, thread_pool_.get());

  // Set connection handler
  acceptor_->SetConnectionHandler([this](int client_fd) { this->HandleConnection(client_fd); });

  // Start accepting connections
  auto start_result = acceptor_->Start();
  if (!start_result) {
    spdlog::error("Failed to start connection acceptor: {}", start_result.error().message());
    return start_result;
  }

  running_.store(true);

  spdlog::info("nvecd server started on {}:{}", config_.api.tcp.bind, acceptor_->GetPort());
  spdlog::info("Ready to accept connections");

  return {};
}

void NvecdServer::Stop() {
  if (!running_.load()) {
    return;
  }

  spdlog::info("Stopping nvecd server...");

  running_.store(false);
  shutdown_.store(true);

  // Stop accepting new connections
  if (acceptor_) {
    acceptor_->Stop();
  }

  // Wait for existing connections to finish (with timeout)
  auto start_time = std::chrono::steady_clock::now();
  while (stats_.active_connections.load() > 0) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time);
    if (elapsed.count() > kShutdownTimeoutMs) {
      spdlog::warn("Shutdown timeout reached with {} active connections", stats_.active_connections.load());
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownCheckIntervalMs));
  }

  // Stop thread pool
  if (thread_pool_) {
    thread_pool_.reset();
  }

  spdlog::info("nvecd server stopped");
  spdlog::info("Total commands processed: {}", stats_.total_commands.load());
  spdlog::info("Total connections: {}", stats_.total_connections.load());
}

utils::Expected<void, utils::Error> NvecdServer::InitializeComponents() {
  spdlog::info("Initializing server components...");

  // Create EventStore
  event_store_ = std::make_unique<events::EventStore>(config_.events);
  spdlog::info("EventStore initialized (buffer_size={})", config_.events.ctx_buffer_size);

  // Create CoOccurrenceIndex
  co_index_ = std::make_unique<events::CoOccurrenceIndex>();
  spdlog::info("CoOccurrenceIndex initialized");

  // Create VectorStore
  vector_store_ = std::make_unique<vectors::VectorStore>(config_.vectors);
  spdlog::info("VectorStore initialized (default_dimension={})", config_.vectors.default_dimension);

  // Create SimilarityEngine
  similarity_engine_ =
      std::make_unique<similarity::SimilarityEngine>(event_store_.get(), co_index_.get(), vector_store_.get(), config_.similarity);
  spdlog::info("SimilarityEngine initialized (fusion: alpha={}, beta={})", config_.similarity.fusion_alpha,
               config_.similarity.fusion_beta);

  // Create SimilarityCache (if enabled)
  if (config_.cache.enabled) {
    cache_ = std::make_unique<cache::SimilarityCache>(config_.cache.max_memory_bytes, config_.cache.min_query_cost_ms);
    spdlog::info("SimilarityCache initialized (max_memory={}MB, min_cost={}ms)",
                 config_.cache.max_memory_bytes / (1024 * 1024), config_.cache.min_query_cost_ms);
  } else {
    cache_ = nullptr;
    spdlog::info("SimilarityCache disabled");
  }

  // Update HandlerContext with initialized components
  handler_ctx_.event_store = event_store_.get();
  handler_ctx_.co_index = co_index_.get();
  handler_ctx_.vector_store = vector_store_.get();
  handler_ctx_.similarity_engine = similarity_engine_.get();
  handler_ctx_.cache = cache_.get();
  handler_ctx_.dump_dir = config_.snapshot.dir;

  // Create snapshot directory if it doesn't exist
  try {
    std::filesystem::create_directories(config_.snapshot.dir);
    spdlog::info("Snapshot directory: {}", config_.snapshot.dir);
  } catch (const std::exception& e) {
    spdlog::warn("Failed to create snapshot directory {}: {}", config_.snapshot.dir, e.what());
  }

  // Create RequestDispatcher
  dispatcher_ = std::make_unique<RequestDispatcher>(handler_ctx_);
  spdlog::info("RequestDispatcher initialized");

  spdlog::info("All components initialized successfully");

  return {};
}

void NvecdServer::HandleConnection(int client_fd) {
  stats_.total_connections++;
  stats_.active_connections++;

  spdlog::debug("New connection: fd={}", client_fd);

  // Submit connection handling to thread pool
  thread_pool_->Submit([this, client_fd]() {
    ConnectionContext conn_ctx;
    conn_ctx.client_fd = client_fd;

    // Create I/O configuration
    IOConfig io_config;
    io_config.recv_buffer_size = 4096;         // 4KB
    io_config.max_query_length = 1024 * 1024;  // 1MB
    io_config.recv_timeout_sec = config_.perf.connection_timeout_sec;

    // Create request processor
    RequestProcessor processor = [this](const std::string& request, ConnectionContext& ctx) {
      return this->ProcessRequest(request, ctx);
    };

    // Create I/O handler and handle connection
    ConnectionIOHandler io_handler(io_config, processor, shutdown_);
    io_handler.HandleConnection(client_fd, conn_ctx);

    stats_.active_connections--;
    spdlog::debug("Connection closed: fd={}", client_fd);
  });
}

std::string NvecdServer::ProcessRequest(const std::string& request, ConnectionContext& conn_ctx) {
  if (!dispatcher_) {
    return "ERROR Server not initialized\r\n";
  }

  return dispatcher_->Dispatch(request, conn_ctx);
}

}  // namespace nvecd::server

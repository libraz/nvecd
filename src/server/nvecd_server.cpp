/**
 * @file nvecd_server.cpp
 * @brief Implementation of NvecdServer
 *
 * Reference: ../mygram-db/src/server/tcp_server.cpp
 * Reusability: 65% (similar patterns, simplified for nvecd)
 */

#include "server/nvecd_server.h"

#include <arpa/inet.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

#include "cache/similarity_cache.h"
#include "server/connection_io_handler.h"
#include "server/http_server.h"
#include "server/reactor_connection.h"
#include "server/request_dispatcher.h"
#include "storage/snapshot_format_v1.h"
#include "storage/wal_checkpoint.h"
#include "utils/error.h"
#include "utils/structured_log.h"

namespace nvecd::server {

namespace {
constexpr int kShutdownCheckIntervalMs = 100;
constexpr size_t kBytesPerMegabyte = 1024 * 1024;  // Bytes in a megabyte

/**
 * @brief Find the newest snapshot in @p dir.
 *
 * Startup recovery must replay the WAL relative to the exact snapshot the WAL
 * was last truncated against. Every snapshot that triggered a truncation wrote a
 * "<snapshot>.walseq" checkpoint sidecar (see WriteWalCheckpoint), so the set of
 * files with a sidecar is precisely the set of valid recovery bases. Among them
 * the newest by modification time is chosen, mirroring the mtime ordering the
 * SnapshotScheduler uses for cleanup. Snapshots without a sidecar are ignored:
 * the WAL was not truncated for them, so replaying the full WAL tail already
 * reconstructs their contents.
 *
 * @param dir Snapshot directory to scan
 * @param require_checkpoint Whether a WAL checkpoint sidecar is mandatory
 * @return Path of the newest eligible snapshot, or std::nullopt if none
 */
std::optional<std::string> FindLatestSnapshot(const std::string& dir, bool require_checkpoint) {
  std::error_code ec;
  std::filesystem::path snapshot_dir(dir);
  if (!std::filesystem::is_directory(snapshot_dir, ec)) {
    return std::nullopt;
  }

  std::optional<std::string> newest_path;
  std::filesystem::file_time_type newest_time{};
  for (const auto& entry : std::filesystem::directory_iterator(snapshot_dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    // The checkpoint sidecar itself must not be treated as a snapshot.
    if (entry.path().extension() == storage::kWalCheckpointSuffix) {
      continue;
    }
    // With WAL enabled, only snapshots with a checkpoint sidecar are valid
    // bases: the sidecar tells us exactly where replay must begin. Without WAL
    // there is no sidecar by design, so the newest valid snapshot is the sole
    // durable recovery source and must not be ignored.
    const std::string sidecar = entry.path().string() + storage::kWalCheckpointSuffix;
    if (require_checkpoint && !std::filesystem::exists(sidecar, ec)) {
      continue;
    }
    auto mtime = std::filesystem::last_write_time(entry, ec);
    if (ec) {
      continue;
    }
    if (!newest_path.has_value() || mtime > newest_time) {
      newest_path = entry.path().string();
      newest_time = mtime;
    }
  }
  return newest_path;
}
}  // namespace

NvecdServer::NvecdServer(config::Config config) : config_(std::move(config)) {}

NvecdServer::~NvecdServer() {
  if (running_.load()) {
    Stop();
  }
}

utils::Expected<void, utils::Error> NvecdServer::Start() {
  if (running_.load()) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kNetworkAlreadyRunning, "Server already running"));
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

  // Start() can fail after worker/reactor construction (for example, an
  // unsupported poller or a bind failure). NvecdServer is not marked running
  // until the listener is live, so Stop() would intentionally be a no-op in
  // this path; clean up the partially-started network stack explicitly.
  const auto abort_network_start = [this] {
    if (unix_acceptor_) {
      unix_acceptor_->Stop();
      unix_acceptor_.reset();
    }
    if (acceptor_) {
      acceptor_->Stop();
      acceptor_.reset();
    }
    if (reactor_) {
      reactor_->Stop();
      reactor_.reset();
    }
    if (thread_pool_) {
      thread_pool_->Shutdown(false, static_cast<uint32_t>(config_.perf.shutdown_timeout_ms));
      thread_pool_.reset();
    }
  };

  // Idle sockets are kept in the reactor, not on worker threads. Workers run
  // only the request callbacks extracted from readable connections.
  ReactorConfig reactor_config;
  reactor_config.idle_timeout_sec = config_.perf.connection_timeout_sec;
  reactor_config.initial_read_timeout_sec = config_.perf.connection_timeout_sec;
  reactor_config.max_total_buffered_bytes = static_cast<size_t>(config_.perf.reactor_max_total_buffered_bytes);
  reactor_ = std::make_unique<IoReactor>(reactor_config);
  reactor_->SetCloseCallback([this](int client_fd) {
    // A descriptor can belong to either listener; RemoveConnection is
    // idempotent, so this keeps both TCP and UDS accounting correct.
    if (acceptor_)
      acceptor_->RemoveConnection(client_fd);
    if (unix_acceptor_)
      unix_acceptor_->RemoveConnection(client_fd);
    stats_.active_connections.fetch_sub(1, std::memory_order_relaxed);
  });
  auto reactor_result = reactor_->Start();
  if (!reactor_result) {
    abort_network_start();
    return reactor_result;
  }

  auto register_connection = [this](int client_fd) {
    IOConfig io_config;
    io_config.recv_buffer_size = static_cast<size_t>(config_.perf.recv_buffer_size);
    io_config.max_query_length = static_cast<size_t>(config_.perf.max_query_length);
    io_config.max_accumulated_bytes = static_cast<size_t>(config_.perf.max_query_length) * 2;
    io_config.recv_timeout_sec = config_.perf.connection_timeout_sec;
    auto connection = ReactorConnection::Create(
        client_fd, reactor_.get(), thread_pool_.get(), io_config,
        [this](const std::string& request, ConnectionContext& context) { return ProcessRequest(request, context); });
    // Increment before Register: the event loop may close a peer immediately
    // after registration and invoke the close callback on another thread.
    stats_.total_connections.fetch_add(1, std::memory_order_relaxed);
    stats_.active_connections.fetch_add(1, std::memory_order_relaxed);
    auto result = reactor_->Register(std::move(connection));
    if (!result) {
      stats_.active_connections.fetch_sub(1, std::memory_order_relaxed);
      spdlog::warn("Failed to register accepted fd with reactor: {}", result.error().message());
      return false;
    }
    return true;
  };

  // Create ServerConfig for connection acceptor
  ServerConfig server_config;
  server_config.host = config_.api.tcp.bind;
  server_config.port = static_cast<uint16_t>(config_.api.tcp.port);
  server_config.max_connections = config_.perf.max_connections;
  server_config.max_connections_per_ip = config_.perf.max_connections_per_ip;
  server_config.worker_threads = worker_threads;
  server_config.recv_buffer_size = config_.perf.recv_buffer_size;
  server_config.send_buffer_size = config_.perf.send_buffer_size;

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
  acceptor_->SetReactorHandler(register_connection);

  // Start accepting connections
  auto start_result = acceptor_->Start();
  if (!start_result) {
    spdlog::error("Failed to start connection acceptor: {}", start_result.error().message());
    abort_network_start();
    return start_result;
  }

  running_.store(true);

  spdlog::info("nvecd server started on {}:{}", config_.api.tcp.bind, acceptor_->GetPort());

  // Start Unix domain socket acceptor (if configured)
  if (!config_.api.unix_socket.path.empty()) {
    ServerConfig uds_config;
    uds_config.unix_socket_path = config_.api.unix_socket.path;
    uds_config.max_connections = config_.perf.max_connections;
    uds_config.recv_buffer_size = config_.perf.recv_buffer_size;
    uds_config.send_buffer_size = config_.perf.send_buffer_size;

    unix_acceptor_ = std::make_unique<ConnectionAcceptor>(uds_config, thread_pool_.get());
    unix_acceptor_->SetReactorHandler(register_connection);

    auto uds_result = unix_acceptor_->Start();
    if (!uds_result) {
      spdlog::warn("Failed to start Unix socket acceptor: {}", uds_result.error().message());
      unix_acceptor_.reset();
      // Non-fatal: TCP continues to work
    } else {
      spdlog::info("Unix domain socket listening on {}", config_.api.unix_socket.path);
    }
  }

  spdlog::info("Ready to accept connections");

  // Start HTTP server if enabled
  if (config_.api.http.enable) {
    HttpServerConfig http_config;
    http_config.bind = config_.api.http.bind;
    http_config.port = config_.api.http.port;
    http_config.read_timeout_sec = config_.api.http.timeout_sec;
    http_config.write_timeout_sec = config_.api.http.timeout_sec;
    http_config.enable_cors = config_.api.http.enable_cors;
    http_config.cors_allow_origin = config_.api.http.cors_allow_origin;
    http_config.allow_cidrs = config_.network.allow_cidrs;
    http_config.requirepass = config_.security.requirepass;
    // Apply the same per-request query-length budget to HTTP and TCP.
    if (config_.perf.max_query_length > 0) {
      http_config.max_payload_bytes = static_cast<size_t>(config_.perf.max_query_length);
    }

    http_server_ =
        std::make_unique<HttpServer>(http_config, &handler_ctx_, &config_, &loading_, &stats_, rate_limiter_.get());

    auto http_start_result = http_server_->Start();
    if (!http_start_result) {
      spdlog::error("Failed to start HTTP server: {}", http_start_result.error().message());
      // Don't fail overall startup if HTTP server fails
      // TCP server is still running
      http_server_.reset();
    } else {
      spdlog::info("HTTP server started on {}:{}", config_.api.http.bind, http_server_->GetPort());
    }
  }

  return {};
}

void NvecdServer::Stop() {
  if (!running_.load()) {
    return;
  }

  spdlog::info("Stopping nvecd server...");

  running_.store(false);
  shutdown_.store(true);

  // Stop snapshot scheduler before waiting for fork child
  if (snapshot_scheduler_) {
    snapshot_scheduler_->Stop();
  }

  // Stop co-occurrence decay scheduler
  if (decay_scheduler_) {
    decay_scheduler_->Stop();
  }

  // Wait for any in-progress fork snapshot
  if (fork_writer_) {
    fork_writer_->WaitForChild();
  }

  // Stop HTTP server
  if (http_server_) {
    http_server_->Stop();
  }

  // Stop Unix domain socket acceptor
  if (unix_acceptor_) {
    unix_acceptor_->Stop();
  }

  // Stop accepting new connections
  if (acceptor_) {
    acceptor_->Stop();
  }

  // The acceptors have stopped admitting sockets. Now unregister all reactor
  // clients before worker shutdown so close callbacks and queued responses
  // still see valid server state.
  if (reactor_) {
    reactor_->Stop();
  }

  // Wait for existing connections to finish (with timeout)
  auto start_time = std::chrono::steady_clock::now();
  while (stats_.active_connections.load() > 0) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time);
    if (elapsed.count() > config_.perf.shutdown_timeout_ms) {
      spdlog::warn("Shutdown timeout reached with {} active connections", stats_.active_connections.load());
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownCheckIntervalMs));
  }

  // Stop thread pool
  if (thread_pool_) {
    // ConnectionAcceptor::Stop has already shut down every active client fd,
    // so connection handlers can observe shutdown and exit.  Bound this final
    // wait by the configured server shutdown deadline instead of letting a
    // misbehaving task make Stop() hang forever.
    thread_pool_->Shutdown(false, static_cast<uint32_t>(config_.perf.shutdown_timeout_ms));
    thread_pool_.reset();
  }
  reactor_.reset();

  // Connection tasks capture their acceptor while removing/closing fds, so
  // keep the UDS acceptor alive until all worker tasks have stopped.
  unix_acceptor_.reset();

  // Close the WAL last: all surfaces (TCP worker threads, HTTP server) are now
  // stopped and the fork child has been reaped, so no write can race the close.
  // Close() flushes pending writes and joins the background sync thread.
  if (wal_.IsOpen()) {
    wal_.Close();
  }

  spdlog::info("nvecd server stopped");
  spdlog::info("Total commands processed: {}", stats_.total_commands.load());
  spdlog::info("Total connections: {}", stats_.total_connections.load());
}

utils::Expected<void, utils::Error> NvecdServer::InitializeComponents() {
  handler_ctx_.snapshot_write_gate = &snapshot_write_gate_;
  handler_ctx_.write_serialization_gate = &write_serialization_gate_;
  spdlog::info("Initializing server components...");

  // Create EventStore
  event_store_ = std::make_unique<events::EventStore>(config_.events);
  spdlog::info("EventStore initialized (buffer_size={})", config_.events.ctx_buffer_size);

  // Create CoOccurrenceIndex with the pruning policy configured alongside
  // the event store. Keeping this wiring here makes the YAML settings apply
  // to both TCP and HTTP ingestion paths through the shared index.
  events::CoOccurrenceIndex::Config co_index_config;
  co_index_config.max_neighbors_per_item = config_.events.max_neighbors_per_item;
  co_index_config.min_support = static_cast<float>(config_.events.min_support);
  co_index_ = std::make_unique<events::CoOccurrenceIndex>(co_index_config);
  spdlog::info("CoOccurrenceIndex initialized");

  // Create VectorStore
  vector_store_ = std::make_unique<vectors::VectorStore>(config_.vectors);
  spdlog::info("VectorStore initialized (default_dimension={})", config_.vectors.default_dimension);

  // Create MetadataStore
  metadata_store_ = std::make_unique<vectors::MetadataStore>();
  spdlog::info("MetadataStore initialized");

  // Create SimilarityEngine
  similarity_engine_ =
      std::make_unique<similarity::SimilarityEngine>(event_store_.get(), co_index_.get(), vector_store_.get(),
                                                     config_.similarity, config_.vectors, metadata_store_.get());
  spdlog::info("SimilarityEngine initialized (fusion: alpha={}, beta={})", config_.similarity.fusion_alpha,
               config_.similarity.fusion_beta);

  // Create SimilarityCache (if enabled)
  if (config_.cache.enabled) {
    cache_ = std::make_unique<cache::SimilarityCache>(config_.cache.max_memory_bytes, config_.cache.min_query_cost_ms,
                                                      config_.cache.ttl_seconds);
    spdlog::info("SimilarityCache initialized (max_memory={}MB, min_cost={}ms, ttl={}s)",
                 config_.cache.max_memory_bytes / kBytesPerMegabyte, config_.cache.min_query_cost_ms,
                 config_.cache.ttl_seconds);
  } else {
    cache_ = nullptr;
    spdlog::info("SimilarityCache disabled");
  }

  // Create RuntimeVariableManager
  auto variable_mgr_result = config::RuntimeVariableManager::Create(config_);
  if (!variable_mgr_result) {
    spdlog::error("Failed to create RuntimeVariableManager: {}", variable_mgr_result.error().message());
    return utils::MakeUnexpected(variable_mgr_result.error());
  }
  variable_manager_ = std::move(*variable_mgr_result);

  // Register cache toggle callback
  variable_manager_->SetCacheToggleCallback([this](bool enabled) -> utils::Expected<void, utils::Error> {
    if (enabled && !cache_) {
      // Create cache if it doesn't exist
      cache_ = std::make_unique<cache::SimilarityCache>(config_.cache.max_memory_bytes, config_.cache.min_query_cost_ms,
                                                        config_.cache.ttl_seconds);
      handler_ctx_.cache.store(cache_.get(), std::memory_order_release);
      spdlog::info("Cache enabled at runtime");
    } else if (!enabled && cache_) {
      // Disable cache (set pointer to null but keep cache for later re-enable)
      handler_ctx_.cache.store(nullptr, std::memory_order_release);
      spdlog::info("Cache disabled at runtime");
    }
    return {};
  });

  // Set SimilarityCache pointer if cache is enabled
  if (cache_) {
    variable_manager_->SetSimilarityCache(cache_.get());
  }

  spdlog::info("RuntimeVariableManager initialized");

  // Update HandlerContext with initialized components
  handler_ctx_.event_store = event_store_.get();
  handler_ctx_.co_index = co_index_.get();
  handler_ctx_.vector_store = vector_store_.get();
  handler_ctx_.metadata_store = metadata_store_.get();
  handler_ctx_.similarity_engine = similarity_engine_.get();
  handler_ctx_.cache.store(cache_.get(), std::memory_order_release);
  handler_ctx_.variable_manager = variable_manager_.get();
  handler_ctx_.dump_dir = config_.snapshot.dir;
  handler_ctx_.requirepass = config_.security.requirepass;

  // Create snapshot directory if it doesn't exist
  std::error_code ec;
  std::filesystem::create_directories(config_.snapshot.dir, ec);
  if (ec) {
    spdlog::warn("Failed to create snapshot directory {}: {}", config_.snapshot.dir, ec.message());
  } else {
    spdlog::info("Snapshot directory: {}", config_.snapshot.dir);
  }

  // Create ForkSnapshotWriter (always create, even in lock mode, for DUMP STATUS)
  fork_writer_ = std::make_unique<storage::ForkSnapshotWriter>();
  handler_ctx_.fork_snapshot_writer = fork_writer_.get();
  spdlog::info("ForkSnapshotWriter initialized (mode: {})", config_.snapshot.mode);

  // Create RequestDispatcher
  dispatcher_ = std::make_unique<RequestDispatcher>(handler_ctx_);
  spdlog::info("RequestDispatcher initialized");

  // Crash recovery (durability) once the stores are populated.
  //
  // Ordering is load-bearing, and handler_ctx_.wal stays null through both the
  // snapshot load AND the WAL replay so neither re-appends to the WAL:
  //   1. Load the latest checkpointed snapshot (if any) into the stores. This
  //      restores all state up to the snapshot's WAL checkpoint sequence; the
  //      WAL was truncated against that snapshot, so its records alone would not
  //      reconstruct the pre-snapshot state.
  //   2. Open() recovers CurrentSequence from existing WAL files.
  //   3. Replay() re-applies records strictly after the snapshot's checkpoint
  //      (floor = ReadWalCheckpoint + 1, or 0 when no snapshot was loaded), so
  //      no event the snapshot already absorbed is double-counted.
  //   4. Only after replay is handler_ctx_.wal published, so live writes append
  //      sequences strictly greater than the recovered maximum.
  //
  // The fork-snapshot writer is also given the WAL pointer so it can write the
  // checkpoint sidecar and truncate the WAL once a background snapshot lands.
  auto load_snapshot = [this](const std::string& snapshot_path) -> utils::Expected<void, utils::Error> {
    config::Config loaded_config;
    storage::snapshot_format::IntegrityError integrity_error;
    auto load =
        storage::snapshot_v1::ReadSnapshotV1(snapshot_path, loaded_config, *event_store_, *co_index_, *vector_store_,
                                             nullptr, nullptr, &integrity_error, metadata_store_.get());
    if (!load) {
      std::string msg = load.error().message();
      if (!integrity_error.message.empty()) {
        msg += " (" + integrity_error.message + ")";
      }
      spdlog::error("Failed to load snapshot {} during recovery: {}", snapshot_path, msg);
      return utils::MakeUnexpected(load.error());
    }
    spdlog::info("Recovery loaded snapshot: {}", snapshot_path);
    return {};
  };

  if (config_.wal.enabled) {
    // 1. Load the latest checkpointed snapshot, if one exists.
    std::string loaded_snapshot_path;
    auto latest = FindLatestSnapshot(config_.snapshot.dir, /*require_checkpoint=*/true);
    if (latest.has_value()) {
      auto load = load_snapshot(*latest);
      if (!load) {
        return utils::MakeUnexpected(load.error());
      }
      loaded_snapshot_path = *latest;
    }

    // 2. Open the WAL (recovers CurrentSequence from existing files).
    storage::WriteAheadLog::Config wal_config;
    wal_config.directory = config_.wal.dir;
    wal_config.max_file_size = config_.wal.max_file_size;
    wal_config.sync_on_write = config_.wal.sync_on_write;
    wal_config.sync_interval_ms = config_.wal.sync_interval_ms;
    wal_config.include_vectors = config_.wal.include_vectors;

    auto wal_open = wal_.Open(wal_config);
    if (!wal_open) {
      // Durability was requested but the WAL is unavailable: fail startup rather
      // than silently degrading to non-durable operation.
      spdlog::error("Failed to open WAL at {}: {}", config_.wal.dir, wal_open.error().message());
      return utils::MakeUnexpected(wal_open.error());
    }

    // 3. Replay only records beyond the loaded snapshot's checkpoint.
    const uint64_t checkpoint = loaded_snapshot_path.empty() ? 0 : storage::ReadWalCheckpoint(loaded_snapshot_path);
    const uint64_t from = (checkpoint == 0) ? 0 : checkpoint + 1;

    // Replay BEFORE publishing handler_ctx_.wal so replayed records are not
    // re-appended.
    auto replayed = wal_.Replay(from, [this](const storage::WalRecord& record) { dispatcher_->ReplayRecord(record); });
    if (!replayed) {
      spdlog::error("WAL replay failed from sequence {}: {}", from, replayed.error().message());
      return utils::MakeUnexpected(replayed.error());
    }
    spdlog::info("WAL replay applied {} record(s) from sequence {}", *replayed, from);

    // 4. Publish the WAL so both surfaces append live writes, and wire it into
    // the fork writer for post-snapshot checkpoint + truncate.
    handler_ctx_.wal = &wal_;
    fork_writer_->SetWal(&wal_);
    spdlog::info("WAL enabled (dir={}, current_sequence={})", config_.wal.dir, wal_.CurrentSequence());
  } else {
    // WAL-off is the default configuration. Its snapshots have no .walseq
    // sidecar, so recover the latest snapshot directly instead of starting
    // every restart from an empty in-memory store.
    auto latest = FindLatestSnapshot(config_.snapshot.dir, /*require_checkpoint=*/false);
    if (latest.has_value()) {
      auto load = load_snapshot(*latest);
      if (!load) {
        return utils::MakeUnexpected(load.error());
      }
    }
  }

  // Snapshot load and WAL replay repopulate the VectorStore directly, bypassing
  // the incremental NotifyVectorAdded path, so the ANN index would otherwise be
  // empty (or hold only replayed tail vectors) after recovery. Rebuild it from
  // the fully restored store so ANN searches cover the entire corpus. No-op for
  // the flat index or an empty store.
  if (similarity_engine_) {
    similarity_engine_->RebuildAnnFromStore();
  }

  // Create and start SnapshotScheduler (if auto-snapshot is enabled)
  if (config_.snapshot.interval_sec > 0) {
    snapshot_scheduler_ =
        std::make_unique<SnapshotScheduler>(config_.snapshot, fork_writer_.get(), &config_, event_store_.get(),
                                            co_index_.get(), vector_store_.get(), metadata_store_.get(), read_only_);
    snapshot_scheduler_->Start();
  }

  // Create and start DecayScheduler (if co-occurrence decay is enabled)
  decay_scheduler_ = std::make_unique<DecayScheduler>(
      co_index_.get(), static_cast<int>(config_.events.decay_interval_sec), config_.events.decay_alpha);
  decay_scheduler_->Start();

  // Create RateLimiter if enabled
  if (config_.api.rate_limiting.enable) {
    rate_limiter_ =
        std::make_unique<RateLimiter>(config_.api.rate_limiting.capacity, config_.api.rate_limiting.refill_rate,
                                      config_.api.rate_limiting.max_clients);
    spdlog::info("RateLimiter initialized (capacity={}, refill_rate={}/s, max_clients={})",
                 config_.api.rate_limiting.capacity, config_.api.rate_limiting.refill_rate,
                 config_.api.rate_limiting.max_clients);
  }

  spdlog::info("All components initialized successfully");

  return {};
}

void NvecdServer::HandleConnection(int client_fd) {
  stats_.total_connections++;
  stats_.active_connections++;

  spdlog::debug("New connection: fd={}", client_fd);

  // This runs synchronously on the thread-pool worker that ConnectionAcceptor
  // already submitted us on. Do NOT submit a second task here: a nested Submit
  // would return immediately, causing the acceptor's outer task to call
  // RemoveConnection() before any I/O ran (undercounting active_fds_ and
  // defeating the connection limits) and would leave the fd unclosed forever
  // (fd leak -> EMFILE -> full-service DoS). Blocking here keeps connection
  // accounting and fd close ownership on the acceptor's task.
  ConnectionContext conn_ctx;
  conn_ctx.client_fd = client_fd;

  // Extract a stable peer identity for rate limiting.  A UNIX-domain peer is
  // not an IPv4 address; interpreting it as one produces a bogus shared key.
  sockaddr_storage peer_addr{};
  socklen_t peer_len = sizeof(peer_addr);
  if (getpeername(client_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len) == 0 &&
      peer_addr.ss_family == AF_INET) {
    const auto* ipv4_peer = reinterpret_cast<const sockaddr_in*>(&peer_addr);
    char ip_buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &ipv4_peer->sin_addr, ip_buf, sizeof(ip_buf)) != nullptr) {
      conn_ctx.client_ip = ip_buf;
    }
  } else if (peer_addr.ss_family == AF_UNIX) {
    // Local socket access is authorized by the socket filesystem permissions.
    // Keep it distinct so ProcessRequest can deliberately exempt it from the
    // per-IP limiter instead of accidentally treating it as an arbitrary IP.
    conn_ctx.client_ip = "unix";
  }

  // Create I/O configuration
  IOConfig io_config;
  io_config.recv_buffer_size = static_cast<size_t>(config_.perf.recv_buffer_size);
  io_config.max_query_length = static_cast<size_t>(config_.perf.max_query_length);
  io_config.max_accumulated_bytes = static_cast<size_t>(config_.perf.max_query_length) * 2;
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
}

std::string NvecdServer::ProcessRequest(const std::string& request, ConnectionContext& conn_ctx) {
  if (!dispatcher_) {
    return "ERROR Server not initialized\r\n";
  }

  // Rate limit check
  if (rate_limiter_ && !conn_ctx.client_ip.empty() && conn_ctx.client_ip != "unix") {
    if (!rate_limiter_->Allow(conn_ctx.client_ip)) {
      return "ERROR Rate limit exceeded\r\n";
    }
  }

  return dispatcher_->Dispatch(request, conn_ctx);
}

}  // namespace nvecd::server

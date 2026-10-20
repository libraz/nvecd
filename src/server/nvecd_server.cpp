/**
 * @file nvecd_server.cpp
 * @brief Implementation of NvecdServer
 *
 * Reference: ../mygram-db/src/server/tcp_server.cpp
 * Reusability: 65% (similar patterns, simplified for nvecd)
 */

#include "server/nvecd_server.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cache/similarity_cache.h"
#include "server/connection_budget.h"
#include "server/handlers/dump_handler.h"
#include "server/http_server.h"
#include "server/request_dispatcher.h"
#include "server/wal_codec.h"
#include "storage/wal_checkpoint.h"
#include "utils/error.h"
#include "utils/network_utils.h"
#include "utils/structured_log.h"

namespace nvecd::server {

namespace {
constexpr int kShutdownCheckIntervalMs = 10;
constexpr size_t kBytesPerMegabyte = 1024 * 1024;  // Bytes in a megabyte
/// Snapshot WAL-off decay maintenance writes inside snapshot.dir.
constexpr const char* kMaintenanceSnapshotFilename = "maintenance.nvec";
}  // namespace

NvecdServer::NvecdServer(config::Config config) : config_(std::move(config)) {}

NvecdServer::~NvecdServer() {
  if (running_.load()) {
    Stop();
  } else {
    AbortStart();
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
    AbortStart();
    return init_result;
  }

  // Resolve the worker count once; TCP and HTTP both size from it.
  int worker_threads = config_.perf.thread_pool_size;
  if (worker_threads <= 0) {
    worker_threads = static_cast<int>(std::thread::hardware_concurrency());
  }
  worker_threads = std::max(1, worker_threads);
  thread_pool_ = std::make_unique<ThreadPool>(static_cast<size_t>(worker_threads));

  spdlog::info("Thread pool created with {} workers", worker_threads);

  // Start() can fail after worker/reactor construction (for example, an
  // unsupported poller or a bind failure). NvecdServer is not marked running
  // until the listener is live, so Stop() would intentionally be a no-op in
  // this path; AbortStart() tears down what was started instead.
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
    AbortStart();
    return reactor_result;
  }

  auto register_connection = [this](int client_fd) {
    IOConfig io_config;
    io_config.recv_buffer_size = static_cast<size_t>(config_.perf.recv_buffer_size);
    io_config.max_query_length = static_cast<size_t>(config_.perf.max_query_length);
    io_config.max_accumulated_bytes = static_cast<size_t>(config_.perf.max_query_length) * 2;
    io_config.recv_timeout_sec = config_.perf.connection_timeout_sec;
    // Increment before Register: the event loop may close a peer immediately
    // after registration and invoke the close callback on another thread.
    stats_.total_connections.fetch_add(1, std::memory_order_relaxed);
    stats_.active_connections.fetch_add(1, std::memory_order_relaxed);
    auto result = reactor_->Register(
        client_fd, thread_pool_.get(), io_config,
        [this](const std::string& request, ConnectionContext& context) { return ProcessRequest(request, context); });
    if (!result) {
      stats_.active_connections.fetch_sub(1, std::memory_order_relaxed);
      spdlog::warn("Failed to register accepted fd with reactor: {}", result.error().message());
      // Registration did not take the fd; the acceptor remains its only closer.
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
  server_config.parsed_allow_cidrs = utils::ParseCidrList(config_.network.allow_cidrs);

  // TCP and Unix socket listeners admit against one connection budget.
  auto connection_budget =
      std::make_shared<ConnectionBudget>(static_cast<size_t>(std::max(0, config_.perf.max_connections)));

  // Create connection acceptor
  acceptor_ = std::make_unique<ConnectionAcceptor>(server_config, thread_pool_.get());
  acceptor_->SetConnectionBudget(connection_budget);

  // Set connection handler
  acceptor_->SetReactorHandler(register_connection);

  // Start accepting connections
  auto start_result = acceptor_->Start();
  if (!start_result) {
    spdlog::error("Failed to start connection acceptor: {}", start_result.error().message());
    AbortStart();
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
    unix_acceptor_->SetConnectionBudget(connection_budget);
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
    http_config.worker_threads = static_cast<size_t>(worker_threads);
    http_config.max_connections = static_cast<size_t>(std::max(0, config_.perf.max_connections));
    http_config.max_connections_per_ip = static_cast<size_t>(std::max(0, config_.perf.max_connections_per_ip));
    const int queued_connections = std::max(1, config_.perf.max_connections - worker_threads);
    http_config.max_queued_connections = static_cast<size_t>(queued_connections);
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

  // Refuses new snapshot sessions before reaping the child, so none can start
  // while the request surfaces below are still draining.
  StopBackgroundWork();

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

  // The acceptors have stopped admitting sockets. Requests already read from
  // live connections finish and their responses flush while the event loop is
  // still running; only then are the connections unregistered.
  if (reactor_) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.perf.shutdown_timeout_ms);
    const auto drained = [this] {
      return in_flight_requests_.load() == 0 &&
             (!thread_pool_ || (thread_pool_->GetQueueSize() == 0 && thread_pool_->GetActiveWorkerCount() == 0)) &&
             reactor_->MemoryBudget()->UsedBytes() == 0;
    };
    while (!drained()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        spdlog::warn("Shutdown timeout reached with {} request(s) in flight", in_flight_requests_.load());
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownCheckIntervalMs));
    }
    // Unregister all reactor clients before worker shutdown so close callbacks
    // and queued responses still see valid server state.
    reactor_->Stop();
  }

  // Stop thread pool. Queued tasks hold raw pointers to the reactor and to the
  // stores destroyed below, so this wait is unbounded on purpose: the acceptors
  // and the reactor are already stopped, no new task can be submitted, and the
  // queue is finite. Returning from Stop() while a worker still runs would let
  // that worker read freed state.
  if (thread_pool_) {
    thread_pool_->Shutdown();
    thread_pool_.reset();
  }
  reactor_.reset();

  // Connection tasks capture their acceptor while removing/closing fds, so
  // keep the UDS acceptor alive until all worker tasks have stopped.
  unix_acceptor_.reset();

  // Close the WAL last: all surfaces (TCP worker threads, HTTP server) are now
  // stopped, and the fork child has been reaped with no new one admitted since
  // shutdown began, so no write can race the close.
  // Close() flushes pending writes and joins the background sync thread.
  if (wal_.IsOpen()) {
    wal_.Close();
  }

  spdlog::info("nvecd server stopped");
  spdlog::info("Total commands processed: {}", stats_.total_commands.load());
  spdlog::info("Total connections: {}", stats_.total_connections.load());
}

void NvecdServer::StopBackgroundWork() {
  // Stop snapshot scheduler before waiting for fork child
  if (snapshot_scheduler_) {
    snapshot_scheduler_->Stop();
  }

  // Stop co-occurrence decay scheduler
  if (decay_scheduler_) {
    decay_scheduler_->Stop();
  }

  // Wait for any in-progress fork snapshot. The grace period is the operator's
  // configured shutdown budget rather than the writer's own default, so a
  // snapshot child cannot hold shutdown open past what the deployment allows.
  // The cast is safe because the schema constrains this key to 100..60000.
  // No DUMP SAVE or DUMP LOAD can start a session after BeginShutdown, so the
  // child waited for here is the last one.
  handler_ctx_.snapshot_coordinator.BeginShutdown();
  if (fork_writer_) {
    fork_writer_->WaitForChild(static_cast<uint32_t>(config_.perf.shutdown_timeout_ms));
  }
}

void NvecdServer::AbortStart() {
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
    thread_pool_->Shutdown(static_cast<uint32_t>(config_.perf.shutdown_timeout_ms));
    thread_pool_.reset();
  }
  // The schedulers reference the WAL, so they are joined before it closes.
  StopBackgroundWork();
  if (wal_.IsOpen()) {
    wal_.Close();
  }
}

utils::Expected<void, utils::Error> NvecdServer::InitializeComponents() {
  handler_ctx_.write_serialization_gate = &write_serialization_gate_;
  spdlog::info("Initializing server components...");

  // Create EventStore
  event_store_ = std::make_unique<events::EventStore>(config_.events);
  spdlog::info("EventStore initialized (buffer_size={})", config_.events.ctx_buffer_size);

  // Create CoOccurrenceIndex with the pruning policy configured alongside
  // the event store. Keeping this wiring here makes the YAML settings apply
  // to both TCP and HTTP ingestion paths through the shared index.
  co_index_ = std::make_unique<events::CoOccurrenceIndex>(handlers::MakeCoOccurrenceConfig(config_.events));
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

  // The controller owns a cache instance for the full server lifetime. A
  // disabled cache is retained but unpublished, so runtime re-enable is always
  // effective and keeps one source of truth across all control surfaces.
  cache_controller_ = std::make_unique<cache::SimilarityCacheController>(
      config_.cache.max_memory_bytes, config_.cache.min_query_cost_ms, config_.cache.ttl_seconds,
      config_.cache.compression_enabled, static_cast<size_t>(config_.cache.eviction_batch_size), config_.cache.enabled,
      &handler_ctx_.cache);
  spdlog::info("SimilarityCache initialized (enabled={}, max_memory={}MB, min_cost={}ms, ttl={}s)",
               config_.cache.enabled, config_.cache.max_memory_bytes / kBytesPerMegabyte,
               config_.cache.min_query_cost_ms, config_.cache.ttl_seconds);

  // Create RuntimeVariableManager
  auto variable_mgr_result = config::RuntimeVariableManager::Create(config_);
  if (!variable_mgr_result) {
    spdlog::error("Failed to create RuntimeVariableManager: {}", variable_mgr_result.error().message());
    return utils::MakeUnexpected(variable_mgr_result.error());
  }
  variable_manager_ = std::move(*variable_mgr_result);

  variable_manager_->SetCacheController(cache_controller_.get());

  spdlog::info("RuntimeVariableManager initialized");

  // Update HandlerContext with initialized components
  handler_ctx_.event_store = event_store_.get();
  handler_ctx_.co_index = co_index_.get();
  handler_ctx_.vector_store = vector_store_.get();
  handler_ctx_.metadata_store = metadata_store_.get();
  handler_ctx_.similarity_engine = similarity_engine_.get();
  handler_ctx_.cache_controller = cache_controller_.get();
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

  // Reclaim temporaries left by a previous run that was killed outright. No
  // destructor runs in that case, and the retention sweep that would otherwise
  // catch them only exists while the scheduler thread does, which the default
  // snapshot interval never starts.
  // No writer exists in this process yet, so a temporary carrying its PID is
  // one a previous run with the same PID left behind.
  ReclaimAbandonedTemporaries(config_.snapshot.dir, /*own_pid_is_abandoned=*/true);

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
  //   2. Open() recovers CurrentSequence from existing WAL files, floored at
  //      the loaded checkpoint.
  //   3. Replay() re-applies records strictly after the snapshot's checkpoint
  //      (from = ReadWalCheckpoint + 1, or 1 when no snapshot was loaded), so
  //      no event the snapshot already absorbed is double-counted.
  //   4. Only after replay is handler_ctx_.wal published, so live writes append
  //      sequences strictly greater than the recovered maximum.
  //
  // The fork-snapshot writer is also given the WAL pointer so it can write the
  // checkpoint sidecar and truncate the WAL once a background snapshot lands.
  auto load_snapshot = [this](const std::string& snapshot_path) -> utils::Expected<void, utils::Error> {
    handlers::StagedStores staged(config_, handler_ctx_.metadata_store != nullptr);
    auto load = staged.Read(snapshot_path);
    if (!load) {
      spdlog::warn("Skipping invalid snapshot {} during recovery: {}", snapshot_path, staged.Describe(load.error()));
      return utils::MakeUnexpected(load.error());
    }
    staged.Publish(handler_ctx_, /*rebuild_ann=*/false);
    spdlog::info("Recovery loaded snapshot: {}", snapshot_path);
    return {};
  };

  struct RecoverySnapshot {
    std::string path;
    uint64_t checkpoint = 0;
  };
  auto load_newest_valid_snapshot = [&](bool require_checkpoint) -> utils::Expected<RecoverySnapshot, utils::Error> {
    std::optional<utils::Error> last_error;
    for (const auto& candidate : storage::ListRecoveryCandidates(config_.snapshot.dir, require_checkpoint)) {
      auto checkpoint = storage::RecoveryCheckpoint(candidate, require_checkpoint);
      if (!checkpoint) {
        spdlog::warn("Skipping snapshot with invalid WAL checkpoint {}: {}", candidate, checkpoint.error().message());
        last_error = checkpoint.error();
        continue;
      }
      auto loaded = load_snapshot(candidate);
      if (loaded) {
        return RecoverySnapshot{candidate, *checkpoint};
      }
      last_error = loaded.error();
    }
    if (last_error.has_value()) {
      return utils::MakeUnexpected(*last_error);
    }
    return RecoverySnapshot{};
  };

  if (config_.wal.enabled) {
    // 1. Load the latest checkpointed snapshot, if one exists.
    auto loaded_snapshot = load_newest_valid_snapshot(/*require_checkpoint=*/true);
    if (!loaded_snapshot) {
      return utils::MakeUnexpected(loaded_snapshot.error());
    }
    const std::string loaded_snapshot_path = loaded_snapshot->path;

    // 2. Open the WAL (recovers CurrentSequence from existing files).
    storage::WriteAheadLog::Config wal_config;
    wal_config.directory = config_.wal.dir;
    wal_config.max_file_size = config_.wal.max_file_size;
    wal_config.sync_on_write = config_.wal.sync_on_write;
    wal_config.sync_interval_ms = config_.wal.sync_interval_ms;

    // The checkpoint floors the sequence so a WAL emptied by truncation never
    // reissues numbers the snapshot already covers.
    const uint64_t checkpoint = loaded_snapshot->checkpoint;
    auto wal_open = wal_.Open(wal_config, checkpoint);
    if (!wal_open) {
      // Durability was requested but the WAL is unavailable: fail startup rather
      // than silently degrading to non-durable operation.
      spdlog::error("Failed to open WAL at {}: {}", config_.wal.dir, wal_open.error().message());
      return utils::MakeUnexpected(wal_open.error());
    }

    // 3. Replay only records beyond the loaded snapshot's checkpoint. Without a
    // snapshot the floor is sequence 1, so a log whose prefix was truncated
    // against a snapshot that is now missing fails instead of replaying a tail.
    const uint64_t from = checkpoint + 1;

    // Replay BEFORE publishing handler_ctx_.wal so replayed records are not
    // re-appended.
    auto replayed =
        wal_.Replay(from, [this](const storage::WalRecord& record) { return dispatcher_->ReplayRecord(record); });
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
    // WAL-off is the default configuration. Its snapshots have no checkpoint
    // sidecar, so recover the latest snapshot directly instead of starting
    // every restart from an empty in-memory store.
    auto loaded_snapshot = load_newest_valid_snapshot(/*require_checkpoint=*/false);
    if (!loaded_snapshot) {
      return utils::MakeUnexpected(loaded_snapshot.error());
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
    snapshot_scheduler_ = std::make_unique<SnapshotScheduler>(config_.snapshot, handler_ctx_);
    snapshot_scheduler_->Start();
  }

  // Create and start DecayScheduler (if co-occurrence decay is enabled)
  decay_scheduler_ = std::make_unique<DecayScheduler>(
      co_index_.get(), static_cast<int>(config_.events.decay_interval_sec), config_.events.decay_alpha,
      [this](double alpha, bool prune) -> utils::Expected<void, utils::Error> {
        {
          auto snapshot_access = handler_ctx_.snapshot_coordinator.AcquireStoreAccess(StoreAccess::kWrite);
          std::lock_guard write_guard(write_serialization_gate_);
          if (read_only_.load(std::memory_order_acquire) ||
              handler_ctx_.durability_failed.load(std::memory_order_acquire)) {
            return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kEventDecayFailed,
                                                          "Server is read-only; decay maintenance skipped"));
          }

          if (handler_ctx_.wal != nullptr) {
            const auto payload = EncodeMaintenance(alpha, prune);
            auto appended =
                handler_ctx_.wal->Append(storage::WalOpType::kCoOccurrenceMaintenance, payload.data(), payload.size());
            if (!appended) {
              handler_ctx_.durability_failed.store(true, std::memory_order_release);
              return utils::MakeUnexpected(appended.error());
            }
          }

          co_index_->ApplyDecay(alpha);
          if (prune) {
            co_index_->Prune();
          }
        }

        // Without a WAL, persist the post-maintenance state through a fork
        // snapshot taken outside the write gates, so restart does not bring
        // decayed or pruned pairs back. A pass that changed nothing is skipped.
        if (handler_ctx_.wal != nullptr || (alpha >= 1.0 && !prune)) {
          return {};
        }
        const auto maintenance_path =
            (std::filesystem::path(config_.snapshot.dir) / kMaintenanceSnapshotFilename).string();
        auto started = handlers::StartForkSnapshot(handler_ctx_, maintenance_path);
        if (!started) {
          // Scores are already decayed in memory; the next pass persists them.
          spdlog::warn("Maintenance snapshot not started: {}", started.error().message());
        }
        return {};
      });
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

std::string NvecdServer::ProcessRequest(const std::string& request, ConnectionContext& conn_ctx) {
  class InFlightRequestGuard {
   public:
    explicit InFlightRequestGuard(std::atomic<size_t>& count) : count_(count) {
      count_.fetch_add(1, std::memory_order_relaxed);
    }
    ~InFlightRequestGuard() { count_.fetch_sub(1, std::memory_order_relaxed); }

    InFlightRequestGuard(const InFlightRequestGuard&) = delete;
    InFlightRequestGuard& operator=(const InFlightRequestGuard&) = delete;

   private:
    std::atomic<size_t>& count_;
  } in_flight(in_flight_requests_);

  if (!dispatcher_) {
    return "ERROR Server not initialized\r\n";
  }

  // Rate limit check
  if (rate_limiter_ && !conn_ctx.client_ip.empty() && conn_ctx.client_ip != "unix") {
    if (!rate_limiter_->Allow(conn_ctx.client_ip)) {
      return dispatcher_->Refuse(request, "Rate limit exceeded");
    }
  }

  return dispatcher_->Dispatch(request, conn_ctx);
}

}  // namespace nvecd::server

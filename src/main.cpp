/**
 * @file main.cpp
 * @brief Entry point for nvecd server
 *
 * Reference: ../mygram-db/src/main.cpp
 * Reusability: 40% (simplified for initial version)
 */

#include <spdlog/spdlog.h>

#include <csignal>
#include <iostream>
#include <thread>

#include "config/config.h"
#include "server/nvecd_server.h"
#include "vectors/distance_simd.h"

namespace {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_shutdown_requested = 0;

// Default configuration constants
constexpr int kDefaultTcpPort = 11017;             // Default nvecd TCP port
constexpr int kDefaultMaxConnections = 10000;      // Default maximum concurrent connections
constexpr int kDefaultConnectionTimeoutSec = 300;  // Default connection timeout (5 minutes)
constexpr int kShutdownPollIntervalMs = 100;       // Main loop poll interval

/**
 * @brief Signal handler for graceful shutdown
 * @param signal Signal number
 *
 * This handler is async-signal-safe: it only sets an atomic flag.
 */
void SignalHandler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    g_shutdown_requested = 1;
  }
}

/**
 * @brief Create default configuration
 *
 * TODO: Replace with config file parsing (config/config.cpp)
 */
nvecd::config::Config CreateDefaultConfig() {
  nvecd::config::Config config;

  // API configuration
  config.api.tcp.bind = "127.0.0.1";
  config.api.tcp.port = kDefaultTcpPort;

  // Performance configuration
  config.perf.thread_pool_size = 0;  // Auto-detect
  config.perf.max_connections = kDefaultMaxConnections;
  config.perf.connection_timeout_sec = kDefaultConnectionTimeoutSec;

  // Events configuration (use defaults from config.h)
  // config.events.ctx_buffer_size = 50;
  // config.events.decay_interval_sec = 3600;
  // config.events.decay_alpha = 0.99;

  // Vectors configuration (use defaults)
  // config.vectors.default_dimension = 768;
  // config.vectors.distance_metric = "cosine";

  // Similarity configuration (use defaults)
  // config.similarity.default_top_k = 100;
  // config.similarity.max_top_k = 1000;
  // config.similarity.fusion_alpha = 0.6;
  // config.similarity.fusion_beta = 0.4;

  // Network configuration - allow localhost for development
  config.network.allow_cidrs = {"127.0.0.1/32"};

  return config;
}

}  // namespace

/**
 * @brief Main entry point
 * @param argc Argument count
 * @param argv Argument values
 * @return Exit code
 */
int main(int argc, char* argv[]) {
  // Setup signal handlers
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  // Setup logging
  spdlog::set_level(spdlog::level::info);  // Use info level for normal operation
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

  // Parse command line arguments
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  bool config_test_mode = false;
  const char* config_path = nullptr;

  // Handle help and version flags first
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [OPTIONS] [<config.yaml>]\n";
      std::cout << "       " << argv[0] << " -c <config.yaml> [OPTIONS]\n";
      std::cout << "\n";
      std::cout << "Options:\n";
      std::cout << "  -c, --config <file>            Configuration file path\n";
      std::cout << "  -t, --config-test              Test configuration file and exit\n";
      std::cout << "  -h, --help                     Show this help message\n";
      std::cout << "  -v, --version                  Show version information\n";
      std::cout << "\n";
      std::cout << "Configuration file format:\n";
      std::cout << "  - YAML (.yaml, .yml) - recommended\n";
      std::cout << "\n";
      std::cout << "Example:\n";
      std::cout << "  " << argv[0] << " -c /etc/nvecd/config.yaml\n";
      std::cout << "  " << argv[0] << " examples/config.yaml\n";
      return 0;
    }
    if (arg == "-v" || arg == "--version") {
      std::cout << "nvecd version 0.1.0\n";
      std::cout << "In-memory vector search engine with event-based co-occurrence tracking\n";
      return 0;
    }
    if (arg == "-t" || arg == "--config-test") {
      config_test_mode = true;
    } else if (arg == "-c" || arg == "--config") {
      if (i + 1 < argc) {
        config_path = argv[++i];
      } else {
        std::cerr << "Error: " << arg << " requires a file path\n";
        return 1;
      }
    } else if (arg[0] != '-') {
      // Positional argument: config file path
      if (config_path == nullptr) {
        config_path = argv[i];
      } else {
        std::cerr << "Error: Multiple config files specified\n";
        return 1;
      }
    } else {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      std::cerr << "Use -h or --help for usage information\n";
      return 1;
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  spdlog::info("nvecd server starting...");
  spdlog::info("Version: 0.1.0");
  spdlog::info("Vector SIMD: {}", nvecd::vectors::simd::GetImplementationName());

  // Load configuration
  nvecd::config::Config config;
  if (config_path != nullptr) {
    spdlog::info("Loading configuration from: {}", config_path);
    auto config_result = nvecd::config::LoadConfig(config_path);
    if (!config_result) {
      spdlog::error("Failed to load config: {}", config_result.error().message());
      return 1;
    }
    config = *config_result;
    spdlog::info("Configuration loaded successfully");

    // Config test mode: validate and exit
    if (config_test_mode) {
      std::cout << "Configuration file is valid\n";
      std::cout << "\nConfiguration summary:\n";
      std::cout << "  Events:\n";
      std::cout << "    ctx_buffer_size: " << config.events.ctx_buffer_size << "\n";
      std::cout << "    decay_interval_sec: " << config.events.decay_interval_sec << "\n";
      std::cout << "    decay_alpha: " << config.events.decay_alpha << "\n";
      std::cout << "  Vectors:\n";
      std::cout << "    default_dimension: " << config.vectors.default_dimension << "\n";
      std::cout << "    distance_metric: " << config.vectors.distance_metric << "\n";
      std::cout << "  Similarity:\n";
      std::cout << "    default_top_k: " << config.similarity.default_top_k << "\n";
      std::cout << "    max_top_k: " << config.similarity.max_top_k << "\n";
      std::cout << "    fusion_alpha: " << config.similarity.fusion_alpha << "\n";
      std::cout << "    fusion_beta: " << config.similarity.fusion_beta << "\n";
      std::cout << "  API:\n";
      std::cout << "    tcp.bind: " << config.api.tcp.bind << "\n";
      std::cout << "    tcp.port: " << config.api.tcp.port << "\n";
      std::cout << "    http.enable: " << (config.api.http.enable ? "true" : "false") << "\n";
      std::cout << "  Performance:\n";
      std::cout << "    thread_pool_size: " << config.perf.thread_pool_size << "\n";
      std::cout << "    max_connections: " << config.perf.max_connections << "\n";
      return 0;
    }
  } else {
    spdlog::info("No configuration file specified, using defaults");
    config = CreateDefaultConfig();
  }

  // Create and start server
  nvecd::server::NvecdServer server(config);

  auto start_result = server.Start();
  if (!start_result) {
    spdlog::error("Failed to start server: {}", start_result.error().message());
    return 1;
  }

  spdlog::info("Server is running. Press Ctrl+C to stop.");
  spdlog::info("Listening on {}:{}", config.api.tcp.bind, server.GetPort());

  // Main loop: wait for shutdown signal
  while (g_shutdown_requested == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownPollIntervalMs));
  }

  spdlog::info("Shutdown signal received");

  // Stop server
  server.Stop();

  spdlog::info("Server stopped gracefully");

  return 0;
}

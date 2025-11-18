/**
 * @file nvecd-cli.cpp
 * @brief Command-line client for nvecd (redis-cli style)
 *
 * Reference: ../mygram-db/src/cli/mygram-cli.cpp
 * Reusability: 75% (adapted for nvecd commands)
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Try to use readline if available
#ifdef HAVE_READLINE
#include <readline/history.h>
#include <readline/readline.h>
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define USE_READLINE 1
#endif

namespace {

// Buffer and string prefix size constants
constexpr size_t kReceiveBufferSize = 65536;  // Receive buffer size (64KB)
constexpr size_t kOkPrefixLength = 3;         // "OK " length
constexpr size_t kErrorPrefixLength = 6;      // "ERROR " length
constexpr int kMaxWaitReadyRetries = 100;     // Maximum retries for --wait-ready (~5 minutes)

#ifdef USE_READLINE
// Command list for tab completion
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-avoid-non-const-global-variables)
const char* command_list[] = {"EVENT", "VECSET", "SIM",  "SIMV", "INFO", "CONFIG", "CACHE",
                              "DUMP",  "DEBUG",  "quit", "exit", "help", nullptr};

/**
 * @brief Command name generator for readline completion
 */
char* CommandGenerator(const char* text, int state) {
  static int list_index;
  static int len;
  const char* name = nullptr;

  if (state == 0) {
    list_index = 0;
    len = static_cast<int>(strlen(text));
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  while ((name = command_list[list_index++]) != nullptr) {
    if (strncasecmp(name, text, len) == 0) {
      // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
      return strdup(name);
    }
  }

  return nullptr;
}

/**
 * @brief Parse the input line and extract tokens
 */
std::vector<std::string> ParseTokens(const std::string& line) {
  std::vector<std::string> tokens;
  std::istringstream iss(line);
  std::string token;
  while (iss >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

/**
 * @brief Generator for keyword completions
 */
char* KeywordGenerator(const std::vector<std::string>& keywords, const char* text, int state) {
  static size_t list_index;
  static int len;

  if (state == 0) {
    list_index = 0;
    len = static_cast<int>(strlen(text));
  }

  while (list_index < keywords.size()) {
    const std::string& name = keywords[list_index++];
    if (len == 0 || strncasecmp(name.c_str(), text, len) == 0) {
      // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
      return strdup(name.c_str());
    }
  }

  return nullptr;
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::vector<std::string> current_keywords;

char* KeywordGeneratorWrapper(const char* text, int state) {
  return KeywordGenerator(current_keywords, text, state);
}

/**
 * @brief Completion function for readline
 */
char** CommandCompletion(const char* text, int start, int /* end */) {
  // Disable default filename completion
  rl_attempted_completion_over = 1;

  // Get the full line buffer up to the cursor
  std::string line(rl_line_buffer, start);
  std::vector<std::string> tokens = ParseTokens(line);

  // First word: complete command name
  if (tokens.empty()) {
    return rl_completion_matches(text, CommandGenerator);
  }

  std::string command = tokens[0];
  // Convert to uppercase for comparison
  for (char& character : command) {
    character = static_cast<char>(toupper(character));
  }

  size_t token_count = tokens.size();

  // EVENT <ctx> <id> <score>
  if (command == "EVENT") {
    if (token_count == 1) {
      current_keywords = {"<context_id>"};
      return rl_completion_matches(text, KeywordGeneratorWrapper);
    }
    if (token_count == 2) {
      current_keywords = {"<item_id>"};
      return rl_completion_matches(text, KeywordGeneratorWrapper);
    }
    if (token_count == 3) {
      current_keywords = {"<score>"};
      return rl_completion_matches(text, KeywordGeneratorWrapper);
    }
    return nullptr;
  }

  // VECSET <id> <f1> <f2> ... <fN>
  if (command == "VECSET") {
    if (token_count == 1) {
      current_keywords = {"<item_id>"};
      return rl_completion_matches(text, KeywordGeneratorWrapper);
    }
    current_keywords = {"<vector_values>"};
    return rl_completion_matches(text, KeywordGeneratorWrapper);
  }

  // SIM <id> <top_k> [using=<mode>]
  if (command == "SIM") {
    if (token_count == 1) {
      current_keywords = {"<item_id>"};
      return rl_completion_matches(text, KeywordGeneratorWrapper);
    }
    if (token_count == 2) {
      current_keywords = {"<top_k>"};
      return rl_completion_matches(text, KeywordGeneratorWrapper);
    }
    if (token_count == 3) {
      current_keywords = {"using=vectors", "using=events", "using=fusion"};
      return rl_completion_matches(text, KeywordGeneratorWrapper);
    }
    return nullptr;
  }

  // SIMV <top_k> <f1> <f2> ... <fN>
  if (command == "SIMV") {
    if (token_count == 1) {
      current_keywords = {"<top_k>"};
      return rl_completion_matches(text, KeywordGeneratorWrapper);
    }
    current_keywords = {"<vector_values>"};
    return rl_completion_matches(text, KeywordGeneratorWrapper);
  }

  // CACHE STATS|CLEAR|ENABLE|DISABLE
  if (command == "CACHE") {
    if (token_count == 1) {
      current_keywords = {"STATS", "CLEAR", "ENABLE", "DISABLE"};
      return rl_completion_matches(text, KeywordGeneratorWrapper);
    }
    return nullptr;
  }

  // DUMP SAVE|LOAD|VERIFY|INFO [filename]
  if (command == "DUMP") {
    if (token_count == 1) {
      current_keywords = {"SAVE", "LOAD", "VERIFY", "INFO"};
      return rl_completion_matches(text, KeywordGeneratorWrapper);
    }
    // Enable filename completion for DUMP commands
    rl_attempted_completion_over = 0;
    return nullptr;
  }

  // DEBUG ON|OFF
  if (command == "DEBUG") {
    if (token_count == 1) {
      current_keywords = {"ON", "OFF"};
      return rl_completion_matches(text, KeywordGeneratorWrapper);
    }
    return nullptr;
  }

  // INFO, CONFIG: no arguments
  if (command == "INFO" || command == "CONFIG") {
    return nullptr;
  }

  return nullptr;
}
#endif

struct Config {
  std::string host = "127.0.0.1";
  uint16_t port = 11017;  // nvecd default port
  bool interactive = true;
  int retry_count = 0;     // Number of retries (0 = no retry)
  int retry_interval = 3;  // Seconds between retries
};

class NvecdClient {
 public:
  NvecdClient(Config config) : config_(std::move(config)) {}

  // Non-copyable (manages socket file descriptor)
  NvecdClient(const NvecdClient&) = delete;
  NvecdClient& operator=(const NvecdClient&) = delete;

  // Movable (default)
  NvecdClient(NvecdClient&&) = default;
  NvecdClient& operator=(NvecdClient&&) = default;

  ~NvecdClient() { Disconnect(); }

  bool Connect() {
    int attempts = 0;
    int max_attempts = 1 + config_.retry_count;

    while (attempts < max_attempts) {
      if (attempts > 0) {
        std::cerr << "\nRetrying in " << config_.retry_interval << " seconds... (attempt " << (attempts + 1) << "/"
                  << max_attempts << ")\n";
        sleep(config_.retry_interval);
      }

      sock_ = socket(AF_INET, SOCK_STREAM, 0);
      if (sock_ < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << '\n';
        attempts++;
        continue;
      }

      struct sockaddr_in server_addr = {};
      server_addr.sin_family = AF_INET;
      server_addr.sin_port = htons(config_.port);

      if (inet_pton(AF_INET, config_.host.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address: " << config_.host << '\n';
        close(sock_);
        sock_ = -1;
        return false;  // Don't retry invalid address
      }

      // POSIX socket API requires sockaddr* type conversion from sockaddr_in*
      if (connect(
              sock_,
              reinterpret_cast<struct sockaddr*>(&server_addr),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
              sizeof(server_addr)) < 0) {
        int saved_errno = errno;
        std::cerr << "Connection failed: " << strerror(saved_errno) << '\n';

        // Provide helpful hints based on error type
        if (saved_errno == ECONNREFUSED) {
          std::cerr << "\nPossible reasons:\n";
          std::cerr << "  1. nvecd server is not running\n";
          std::cerr << "  2. Server is still initializing (loading snapshot)\n";
          std::cerr << "  3. Wrong port (check config.yaml - default is 11017)\n";
          std::cerr << "\nTo check server status:\n";
          std::cerr << "  ps aux | grep nvecd\n";
          std::cerr << "  lsof -i -P | grep LISTEN | grep " << config_.port << "\n";
        } else if (saved_errno == ETIMEDOUT) {
          std::cerr << "\nServer is not responding. Check if the server is running and network is accessible.\n";
        } else if (saved_errno == ENETUNREACH || saved_errno == EHOSTUNREACH) {
          std::cerr << "\nNetwork is unreachable. Check hostname and network connectivity.\n";
        }

        close(sock_);
        sock_ = -1;

        // Only retry on ECONNREFUSED (server not ready yet)
        if (saved_errno != ECONNREFUSED) {
          return false;
        }

        attempts++;
        continue;
      }

      // Connected successfully
      if (attempts > 0) {
        std::cerr << "\nConnected successfully after " << attempts << " retry(ies)!\n\n";
      }
      return true;
    }

    return false;
  }

  void Disconnect() {
    if (sock_ >= 0) {
      close(sock_);
      sock_ = -1;
    }
  }

  [[nodiscard]] bool IsConnected() const { return sock_ >= 0; }

  [[nodiscard]] std::string SendCommand(const std::string& command) const {
    if (!IsConnected()) {
      return "(error) Not connected";
    }

    // Send command with \n
    std::string msg = command + "\n";
    ssize_t sent = send(sock_, msg.c_str(), msg.length(), 0);
    if (sent < 0) {
      int saved_errno = errno;
      if (saved_errno == EPIPE || saved_errno == ECONNRESET) {
        return "(error) SERVER_DISCONNECTED: Connection lost while sending command. The server may have crashed or "
               "been shut down.";
      }
      return "(error) Failed to send command: " + std::string(strerror(saved_errno));
    }

    // Receive response (may need multiple recv calls for complete response)
    std::string response;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    char buffer[kReceiveBufferSize];

    // Keep reading until we get a complete response (ends with \n)
    while (true) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
      ssize_t received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
      if (received <= 0) {
        if (received == 0) {
          return "(error) SERVER_DISCONNECTED: Server closed the connection. This usually means:\n"
                 "  1. Server was shut down gracefully\n"
                 "  2. Server crashed or encountered a fatal error\n"
                 "  3. Server restarted and dropped all connections\n"
                 "\nTry reconnecting to check if the server is still running.";
        }
        int saved_errno = errno;
        if (saved_errno == ECONNRESET) {
          return "(error) SERVER_DISCONNECTED: Connection reset by server. The server may have crashed.";
        }
        if (saved_errno == ETIMEDOUT) {
          return "(error) SERVER_TIMEOUT: Server did not respond in time. It may be under heavy load or frozen.";
        }
        return "(error) Failed to receive response: " + std::string(strerror(saved_errno));
      }

      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      buffer[received] = '\0';
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
      response += buffer;

      // Check if we have a complete response (ends with \n)
      if (!response.empty() && response.back() == '\n') {
        break;
      }
    }

    // Remove trailing newline
    if (!response.empty() && response.back() == '\n') {
      response.pop_back();
    }
    // Also remove \r if present
    if (!response.empty() && response.back() == '\r') {
      response.pop_back();
    }

    return response;
  }

  void RunInteractive() const {
    std::cout << "nvecd-cli " << config_.host << ":" << config_.port << '\n';
#ifdef USE_READLINE
    std::cout << "Type 'quit' or 'exit' to exit, 'help' for help" << '\n';
    std::cout << "Use TAB for context-aware command completion" << '\n';
#else
    std::cout << "Type 'quit' or 'exit' to exit, 'help' for help" << '\n';
#endif
    std::cout << '\n';

#ifdef USE_READLINE
    // Setup readline completion
    rl_attempted_completion_function = CommandCompletion;
#endif

    while (true) {
      // Read command
      std::string line;

#ifdef USE_READLINE
      // Use readline for better line editing and history
      std::string prompt = config_.host + ":" + std::to_string(config_.port) + "> ";
      // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
      char* raw_input = readline(prompt.c_str());

      if (raw_input == nullptr) {
        // EOF (Ctrl-D)
        std::cout << '\n';
        break;
      }

      // Use RAII to ensure memory is freed even if exception occurs
      // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
      std::unique_ptr<char, decltype(&free)> input(raw_input, &free);

      line = input.get();

      // Trim whitespace
      line.erase(0, line.find_first_not_of(" \t\r\n"));
      auto pos = line.find_last_not_of(" \t\r\n");
      if (pos != std::string::npos) {
        line.erase(pos + 1);
      }

      // Add to history if non-empty
      if (!line.empty()) {
        add_history(input.get());
      }
      // input is automatically freed by unique_ptr destructor
#else
      // Fallback to std::getline
      std::cout << config_.host << ":" << config_.port << "> ";
      std::cout.flush();

      if (!std::getline(std::cin, line)) {
        break;  // EOF
      }

      // Trim whitespace
      line.erase(0, line.find_first_not_of(" \t\r\n"));
      auto pos = line.find_last_not_of(" \t\r\n");
      if (pos != std::string::npos) {
        line.erase(pos + 1);
      }
#endif

      if (line.empty()) {
        continue;
      }

      // Check for exit commands
      if (line == "quit" || line == "exit") {
        std::cout << "Bye!" << '\n';
        break;
      }

      // Check for help
      if (line == "help") {
        PrintHelp();
        continue;
      }

      // Send command and print response
      std::string response = SendCommand(line);

      // Check if server disconnected
      if (response.find("SERVER_DISCONNECTED") != std::string::npos ||
          response.find("SERVER_TIMEOUT") != std::string::npos) {
        PrintResponse(response);
        std::cout << "\nConnection to server lost. Exiting...\n";
        break;
      }

      PrintResponse(response);
    }
  }

  void RunSingleCommand(const std::string& command) const {
    std::string response = SendCommand(command);
    PrintResponse(response);
  }

 private:
  static void PrintHelp() {
    std::cout << "Available commands:" << '\n';
    std::cout << "  EVENT <ctx> <id> <score>          - Track user behavior event" << '\n';
    std::cout << "  VECSET <id> <f1> <f2> ... <fN>    - Register or update vector" << '\n';
    std::cout << "  SIM <id> <top_k> [using=<mode>]   - Search similar items by ID" << '\n';
    std::cout << "  SIMV <top_k> <f1> <f2> ... <fN>   - Search similar items by vector" << '\n';
    std::cout << "  INFO                               - Show server statistics" << '\n';
    std::cout << "  CONFIG                             - Show current configuration" << '\n';
    std::cout << "  CACHE STATS                        - Show cache statistics" << '\n';
    std::cout << "  CACHE CLEAR                        - Clear all cache entries" << '\n';
    std::cout << "  DUMP SAVE [filename]               - Save snapshot to disk" << '\n';
    std::cout << "  DUMP LOAD <filename>               - Load snapshot from disk" << '\n';
    std::cout << "  DUMP VERIFY <filename>             - Verify snapshot integrity" << '\n';
    std::cout << "  DUMP INFO <filename>               - Show snapshot metadata" << '\n';
    std::cout << "  DEBUG ON                           - Enable debug mode" << '\n';
    std::cout << "  DEBUG OFF                          - Disable debug mode" << '\n';
    std::cout << '\n';
    std::cout << "Search modes (for SIM command):" << '\n';
    std::cout << "  using=vectors  - Content-based similarity (default)" << '\n';
    std::cout << "  using=events   - Behavior-based similarity (co-occurrence)" << '\n';
    std::cout << "  using=fusion   - Hybrid: vectors + events" << '\n';
    std::cout << '\n';
    std::cout << "Examples:" << '\n';
    std::cout << "  EVENT user_alice product123 100              # Track purchase" << '\n';
    std::cout << "  VECSET product123 0.1 0.2 0.3 0.4            # Register 4-dim vector" << '\n';
    std::cout << "  SIM product123 10 using=vectors              # Top-10 content similar" << '\n';
    std::cout << "  SIM product123 10 using=fusion               # Top-10 hybrid" << '\n';
    std::cout << "  SIMV 10 0.5 0.3 0.2 0.1                      # Search by query vector" << '\n';
    std::cout << '\n';
    std::cout << "Other commands:" << '\n';
    std::cout << "  quit/exit - Exit the client" << '\n';
    std::cout << "  help      - Show this help" << '\n';
  }

  static void PrintResponse(const std::string& response) {
    // Parse response type
    if (response.find("OK RESULTS") == 0) {
      // SIM/SIMV response: OK RESULTS <count> <id1> <score1> <id2> <score2> ...
      std::istringstream iss(response);
      std::string status;
      std::string results_str;
      size_t count = 0;
      iss >> status >> results_str >> count;

      std::vector<std::pair<std::string, float>> items;
      std::string id;
      float score = 0.0f;

      while (iss >> id >> score) {
        items.emplace_back(id, score);
      }

      std::cout << "(" << count << " results";
      if (!items.empty()) {
        std::cout << ", showing " << items.size() << ")";
      } else {
        std::cout << ")";
      }
      std::cout << '\n';

      for (size_t i = 0; i < items.size(); ++i) {
        std::cout << (i + 1) << ") " << items[i].first << " (score: " << items[i].second << ")\n";
      }
    } else if (response.find("OK DEBUG_ON") == 0) {
      std::cout << "Debug mode enabled" << '\n';
    } else if (response.find("OK DEBUG_OFF") == 0) {
      std::cout << "Debug mode disabled" << '\n';
    } else if (response.find("OK") == 0) {
      // Generic OK response - print everything after "OK "
      if (response.length() > kOkPrefixLength) {
        std::cout << response.substr(kOkPrefixLength) << '\n';
      } else {
        std::cout << "OK" << '\n';
      }
    } else if (response.find("ERROR") == 0) {
      // Error response
      std::cout << "(error) " << response.substr(kErrorPrefixLength) << '\n';
    } else {
      // Unknown response
      std::cout << response << '\n';
    }

    // CRITICAL: Explicit flush required for popen() to capture output
    // Short responses like "OK\n" may not trigger automatic flush before process exit
    std::cout << std::flush;
  }

  Config config_;
  int sock_{-1};
};

void PrintUsage(const char* program_name) {
  std::cout << "Usage: " << program_name << " [OPTIONS] [COMMAND]" << '\n';
  std::cout << '\n';
  std::cout << "Options:" << '\n';
  std::cout << "  -h HOST         Server hostname (default: 127.0.0.1)" << '\n';
  std::cout << "  -p PORT         Server port (default: 11017)" << '\n';
  std::cout << "  --retry N       Retry connection N times if refused (default: 0)" << '\n';
  std::cout << "  --wait-ready    Keep retrying until server is ready (max 100 attempts)" << '\n';
  std::cout << "  --help          Show this help" << '\n';
  std::cout << '\n';
  std::cout << "Examples:" << '\n';
  std::cout << "  " << program_name << "                           # Interactive mode" << '\n';
  std::cout << "  " << program_name << " -h localhost -p 11017     # Connect to specific server" << '\n';
  std::cout << "  " << program_name << " --retry 5 INFO            # Retry 5 times if server not ready" << '\n';
  std::cout << "  " << program_name << " --wait-ready INFO         # Wait until server is ready" << '\n';
  std::cout << "  " << program_name << " SIM product123 10         # Execute single command" << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
  // CRITICAL: Line-buffered stdout ensures output is flushed on newlines
  // Required for popen() to capture output before process termination
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  Config config;
  std::vector<std::string> command_args;

  // Parse arguments
  for (int i = 1; i < argc; ++i) {
    // Standard C main() argv access requires pointer arithmetic
    std::string arg(argv[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    if (arg == "--help") {
      PrintUsage(argv[0]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      return 0;
    }
    if (arg == "-h") {
      if (i + 1 < argc) {
        config.host = argv[++i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      } else {
        std::cerr << "Error: -h requires an argument" << '\n';
        return 1;
      }
    } else if (arg == "-p") {
      if (i + 1 < argc) {
        try {
          config.port =
              static_cast<uint16_t>(std::stoi(argv[++i]));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        } catch (const std::exception& e) {
          std::cerr << "Error: Invalid port number" << '\n';
          return 1;
        }
      } else {
        std::cerr << "Error: -p requires an argument" << '\n';
        return 1;
      }
    } else if (arg == "--retry") {
      if (i + 1 < argc) {
        try {
          config.retry_count = std::stoi(argv[++i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
          if (config.retry_count < 0) {
            std::cerr << "Error: --retry value must be non-negative" << '\n';
            return 1;
          }
        } catch (const std::exception& e) {
          std::cerr << "Error: Invalid retry count" << '\n';
          return 1;
        }
      } else {
        std::cerr << "Error: --retry requires an argument" << '\n';
        return 1;
      }
    } else if (arg == "--wait-ready") {
      config.retry_count = kMaxWaitReadyRetries;  // Max retries for --wait-ready (~5 minutes)
    } else {
      // Assume remaining args are a command
      for (int j = i; j < argc; ++j) {
        command_args.emplace_back(argv[j]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      }
      config.interactive = false;
      break;
    }
  }

  // Create client and connect
  NvecdClient client(config);
  if (!client.Connect()) {
    return 1;
  }

  // Run interactive or single command mode
  if (config.interactive) {
    client.RunInteractive();
  } else {
    // Build command from args
    std::ostringstream command;
    for (size_t i = 0; i < command_args.size(); ++i) {
      if (i > 0) {
        command << " ";
      }
      command << command_args[i];
    }
    client.RunSingleCommand(command.str());
  }

  return 0;
}

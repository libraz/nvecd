/**
 * @file nvecdclient.cpp
 * @brief Implementation of nvecd client library
 *
 * Reference: ../mygram-db/src/client/mygramclient.cpp
 * Reusability: 90% (connection management, socket handling, protocol parsing)
 * Adapted for: nvecd-specific commands (EVENT, VECSET, SIM, SIMV)
 */

#include "client/nvecdclient.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cctype>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "client/protocol_transport.h"
#include "utils/error.h"
#include "utils/expected.h"

using namespace nvecd::utils;

namespace nvecd::client {

namespace {

// Protocol constants
constexpr size_t kErrorPrefixLen = 6;  // Length of "ERROR "
constexpr int kMillisecondsPerSecond = 1000;
constexpr int kMicrosecondsPerMillisecond = 1000;

// Round-trippable precision for IEEE-754 single precision floats: a 9-digit
// decimal mantissa uniquely identifies any float, so serialized vectors parse
// back to the exact same value on the server.
constexpr int kFloatRoundTripPrecision = 9;

/**
 * @brief Parse an unsigned 64-bit integer without throwing.
 *
 * Replaces std::stoull, which throws on malformed input. On a parse failure the
 * supplied default is returned, keeping the no-exceptions contract intact (this
 * matters across the C ABI boundary, where a thrown exception is undefined).
 *
 * @param value Text to parse.
 * @param fallback Value returned when @p value is not a valid integer.
 * @return Parsed integer, or @p fallback on failure.
 */
uint64_t ParseUint64(const std::string& value, uint64_t fallback = 0) {
  uint64_t parsed = 0;
  const char* begin = value.data();
  const char* end = begin + value.size();
  auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (ec != std::errc() || ptr != end) {
    return fallback;
  }
  return parsed;
}

/**
 * @brief Serialize a float with full round-trippable precision.
 */
std::string FormatFloat(float value) {
  std::ostringstream oss;
  oss << std::setprecision(kFloatRoundTripPrecision) << value;
  return oss.str();
}

/**
 * @brief Append SIM/SIMV optional tokens (filter/min_score) to a command.
 *
 * @c adaptive is intentionally not handled here because its position differs
 * between SIM and SIMV; callers append it explicitly where supported.
 */
void AppendSearchOptions(std::ostringstream& cmd, const SearchOptions& options) {
  if (!options.filter.empty()) {
    cmd << " filter=" << options.filter;
  }
  if (options.min_score.has_value()) {
    cmd << " min_score=" << FormatFloat(*options.min_score);
  }
}

/**
 * @brief Validate that a string does not contain ASCII control characters
 */
std::optional<std::string> ValidateNoControlCharacters(const std::string& value, const char* field_name) {
  for (unsigned char character : value) {
    if (std::iscntrl(character) != 0) {
      std::ostringstream oss;
      oss << "Input for " << field_name << " contains control character 0x" << std::uppercase << std::hex
          << std::setw(2) << std::setfill('0') << static_cast<int>(character) << ", which is not allowed";
      return oss.str();
    }
  }

  return std::nullopt;
}

/**
 * @brief Validate a whitespace-delimited protocol token.
 *
 * nvecd does not implement shell-style quoting. Reject whitespace rather than
 * serializing a command that would be parsed as different arguments. AUTH is
 * deliberately excluded because its password is an opaque suffix.
 */
std::optional<std::string> ValidateProtocolToken(const std::string& value, const char* field_name) {
  if (auto error = ValidateNoControlCharacters(value, field_name)) {
    return error;
  }
  for (unsigned char character : value) {
    if (std::isspace(character) != 0) {
      return std::string("Input for ") + field_name + " must not contain whitespace";
    }
  }
  return std::nullopt;
}

}  // namespace

namespace detail {

std::optional<size_t> CompleteResponseLength(const std::string& buffer, bool requires_end_terminator) {
  transport::ResponseFrame frame;
  frame.end_terminated = requires_end_terminator;
  return transport::CompleteResponseLength(buffer, frame);
}

bool IsResponseComplete(const std::string& buffer) {
  return CompleteResponseLength(buffer).has_value();
}

}  // namespace detail

/**
 * @brief PIMPL implementation class
 */
class NvecdClient::Impl {
 public:
  explicit Impl(ClientConfig config) : config_(std::move(config)) {}

  ~Impl() { Disconnect(); }

  // Non-copyable/non-movable: the public object moves its owning unique_ptr,
  // while this implementation retains a stable round-trip mutex address.
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  Expected<void, Error> Connect() {
    std::lock_guard lock(round_trip_mutex_);
    if (sock_ >= 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientAlreadyConnected, "Already connected"));
    }

    // Unix domain socket connection
    if (!config_.unix_socket_path.empty()) {
      sock_ = socket(AF_UNIX, SOCK_STREAM, 0);
      if (sock_ < 0) {
        return MakeUnexpected(MakeError(ErrorCode::kClientConnectionFailed,
                                        "Failed to create unix socket: " + std::string(strerror(errno))));
      }
      (void)transport::ConfigureNoSigpipe(sock_);

      // Set timeouts
      struct timeval timeout_val = {};
      timeout_val.tv_sec = static_cast<decltype(timeout_val.tv_sec)>(config_.timeout_ms / kMillisecondsPerSecond);
      timeout_val.tv_usec = static_cast<decltype(timeout_val.tv_usec)>((config_.timeout_ms % kMillisecondsPerSecond) *
                                                                       kMicrosecondsPerMillisecond);
      (void)setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &timeout_val, sizeof(timeout_val));
      (void)setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &timeout_val, sizeof(timeout_val));

      struct sockaddr_un server_addr = {};
      server_addr.sun_family = AF_UNIX;
      if (config_.unix_socket_path.size() >= sizeof(server_addr.sun_path)) {
        close(sock_);
        sock_ = -1;
        return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Unix socket path is too long"));
      }
      std::memcpy(server_addr.sun_path, config_.unix_socket_path.c_str(), config_.unix_socket_path.size() + 1);

      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Required for socket API
      if (connect(sock_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::string err = "Unix socket connection failed: " + std::string(strerror(errno));
        close(sock_);
        sock_ = -1;
        return MakeUnexpected(MakeError(ErrorCode::kClientConnectionFailed, err));
      }

      return {};  // Skip TCP connection code
    }

    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
      return MakeUnexpected(
          MakeError(ErrorCode::kClientConnectionFailed, std::string("Failed to create socket: ") + strerror(errno)));
    }
    (void)transport::ConfigureNoSigpipe(sock_);

    // Set socket timeout
    struct timeval timeout_val = {};
    timeout_val.tv_sec = static_cast<decltype(timeout_val.tv_sec)>(config_.timeout_ms / kMillisecondsPerSecond);
    timeout_val.tv_usec = static_cast<decltype(timeout_val.tv_usec)>((config_.timeout_ms % kMillisecondsPerSecond) *
                                                                     kMicrosecondsPerMillisecond);
    // Non-critical: timeout setting failure is acceptable (connection still works, just without timeout)
    (void)setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &timeout_val, sizeof(timeout_val));
    (void)setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &timeout_val, sizeof(timeout_val));

    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config_.port);

    if (inet_pton(AF_INET, config_.host.c_str(), &server_addr.sin_addr) <= 0) {
      close(sock_);
      sock_ = -1;
      return MakeUnexpected(MakeError(ErrorCode::kClientConnectionFailed, "Invalid address: " + config_.host));
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - Required for socket API
    if (connect(sock_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
      std::string error_msg = std::string("Connection failed: ") + strerror(errno);
      close(sock_);
      sock_ = -1;
      return MakeUnexpected(MakeError(ErrorCode::kClientConnectionFailed, error_msg));
    }

    return {};
  }

  void Disconnect() const {
    std::lock_guard lock(round_trip_mutex_);
    DisconnectUnlocked();
  }

  void DisconnectUnlocked() const {
    if (sock_ >= 0) {
      close(sock_);
      sock_ = -1;
    }
    response_buffer_.clear();
    debug_mode_ = false;
  }

  [[nodiscard]] bool IsConnected() const {
    std::lock_guard lock(round_trip_mutex_);
    return sock_ >= 0;
  }

  Expected<std::string, Error> SendCommand(const std::string& command) const {
    std::lock_guard lock(round_trip_mutex_);
    if (sock_ < 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientNotConnected, "Not connected"));
    }

    // Send command with \r\n terminator
    std::string msg = command + "\r\n";
    int io_error = 0;
    if (!transport::SendAll(sock_, msg, &io_error)) {
      DisconnectUnlocked();
      return MakeUnexpected(
          MakeError(ErrorCode::kClientCommandFailed, std::string("Failed to send command: ") + strerror(io_error)));
    }

    // Receive response. A single recv() may deliver only part of a multi-line
    // SIM/SIMV response (or coalesce several responses), so loop until length-
    // aware framing reports a complete response. See detail::IsResponseComplete.
    std::string response;
    std::vector<char> buffer(config_.recv_buffer_size);

    const auto frame = transport::FrameForCommand(command, debug_mode_);
    while (true) {
      if (const auto response_length = transport::CompleteResponseLength(response_buffer_, frame)) {
        response.assign(response_buffer_.data(), *response_length);
        response_buffer_.erase(0, *response_length);
        break;
      }
      const ssize_t received = transport::ReceiveRetryingEintr(sock_, buffer.data(), buffer.size(), &io_error);
      if (received <= 0) {
        DisconnectUnlocked();
        if (received == 0) {
          return MakeUnexpected(MakeError(ErrorCode::kClientConnectionClosed, "Connection closed by server"));
        }
        return MakeUnexpected(MakeError(ErrorCode::kClientCommandFailed,
                                        std::string("Failed to receive response: ") + strerror(io_error)));
      }

      response_buffer_.append(buffer.data(), static_cast<size_t>(received));
      constexpr size_t kMaxBufferedResponseBytes = 64U * 1024U * 1024U;
      if (response_buffer_.size() > kMaxBufferedResponseBytes) {
        DisconnectUnlocked();
        return MakeUnexpected(MakeError(ErrorCode::kClientProtocolError, "Response exceeds 64 MiB limit"));
      }
    }

    // Remove trailing \r\n
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r')) {
      response.pop_back();
    }

    if (!IsErrorResponse(response)) {
      const std::string upper = transport::UpperCommand(command);
      if (upper == "DEBUG ON") {
        debug_mode_ = true;
      } else if (upper == "DEBUG OFF") {
        debug_mode_ = false;
      }
    }

    return response;
  }

  //
  // nvecd-specific commands
  //

  Expected<void, Error> Event(const std::string& ctx, const std::string& type, const std::string& id, int score) const {
    if (auto err = ValidateProtocolToken(ctx, "context ID")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (auto err = ValidateNoControlCharacters(type, "event type")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (auto err = ValidateProtocolToken(id, "document ID")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }

    // Validate type
    if (type != "ADD" && type != "SET" && type != "DEL") {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Event type must be ADD, SET, or DEL"));
    }

    // Validate score for ADD/SET
    if ((type == "ADD" || type == "SET") && (score < 0 || score > 100)) {  // NOLINT(readability-magic-numbers)
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Score must be between 0 and 100"));
    }

    std::ostringstream cmd;
    cmd << "EVENT " << ctx << " " << type << " " << id;
    if (type != "DEL") {
      cmd << " " << score;
    }

    auto result = SendCommand(cmd.str());
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    if (result->find("OK") != 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientProtocolError, "Unexpected response: " + *result));
    }

    return {};
  }

  Expected<void, Error> Vecset(const std::string& id, const std::vector<float>& vector) const {
    if (auto err = ValidateProtocolToken(id, "vector ID")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (vector.empty()) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Vector cannot be empty"));
    }

    std::ostringstream cmd;
    cmd << "VECSET " << id;
    for (float val : vector) {
      cmd << " " << FormatFloat(val);
    }

    auto result = SendCommand(cmd.str());
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    if (result->find("OK") != 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientProtocolError, "Unexpected response: " + *result));
    }

    return {};
  }

  Expected<void, Error> Vecdel(const std::string& id) const {
    if (auto err = ValidateProtocolToken(id, "vector ID")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (id.empty()) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Vector ID cannot be empty"));
    }

    auto result = SendCommand("VECDEL " + id);
    if (!result) {
      return MakeUnexpected(result.error());
    }
    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }
    if (result->find("OK") != 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientProtocolError, "Unexpected response: " + *result));
    }
    return {};
  }

  Expected<void, Error> Metaset(const std::string& id, const std::string& metadata) const {
    if (auto err = ValidateProtocolToken(id, "item ID")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (auto err = ValidateProtocolToken(metadata, "metadata")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (metadata.empty()) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Metadata cannot be empty"));
    }

    std::ostringstream cmd;
    cmd << "METASET " << id << " " << metadata;

    auto result = SendCommand(cmd.str());
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    if (result->find("OK") != 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientProtocolError, "Unexpected response: " + *result));
    }

    return {};
  }

  Expected<SimResponse, Error> Sim(const std::string& id, uint32_t top_k, const std::string& mode,
                                   const SearchOptions& options) const {
    if (auto err = ValidateProtocolToken(id, "document ID")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (auto err = ValidateProtocolToken(mode, "search mode")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (auto err = ValidateProtocolToken(options.filter, "metadata filter")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }

    std::ostringstream cmd;
    cmd << "SIM " << id << " " << top_k;
    if (!mode.empty()) {
      cmd << " using=" << mode;
    }
    AppendSearchOptions(cmd, options);
    if (options.adaptive.has_value()) {
      cmd << " adaptive=" << (*options.adaptive ? "on" : "off");
    }

    auto result = SendCommand(cmd.str());
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    // Parse response: OK RESULTS <count> <id1> <score1> <id2> <score2> ...
    if (result->find("OK RESULTS") != 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientProtocolError, "Unexpected response format"));
    }

    std::istringstream iss(*result);
    std::string status;
    std::string results_str;
    int count = 0;
    iss >> status >> results_str >> count;

    SimResponse resp;
    resp.mode = mode.empty() ? "fusion" : mode;

    // Parse result pairs (id score id score ...)
    std::string result_id;
    float score = 0.0F;
    while (iss >> result_id >> score) {
      resp.results.emplace_back(result_id, score);
    }

    return resp;
  }

  Expected<SimResponse, Error> Simv(const std::vector<float>& vector, uint32_t top_k, const std::string& mode,
                                    const SearchOptions& options) const {
    if (vector.empty()) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Vector cannot be empty"));
    }
    if (auto err = ValidateProtocolToken(mode, "search mode")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (auto err = ValidateProtocolToken(options.filter, "metadata filter")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }

    // SIMV wire syntax places filter=/min_score= before the vector components.
    std::ostringstream cmd;
    cmd << "SIMV " << top_k;
    AppendSearchOptions(cmd, options);
    for (float val : vector) {
      cmd << " " << FormatFloat(val);
    }

    auto result = SendCommand(cmd.str());
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    // Parse response: OK RESULTS <count> <id1> <score1> <id2> <score2> ...
    if (result->find("OK RESULTS") != 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientProtocolError, "Unexpected response format"));
    }

    std::istringstream iss(*result);
    std::string status;
    std::string results_str;
    int count = 0;
    iss >> status >> results_str >> count;

    SimResponse resp;
    resp.mode = mode.empty() ? "vectors" : mode;

    // Parse result pairs (id score id score ...)
    std::string result_id;
    float score = 0.0F;
    while (iss >> result_id >> score) {
      resp.results.emplace_back(result_id, score);
    }

    return resp;
  }

  //
  // MygramDB-compatible commands
  //

  Expected<ServerInfo, Error> Info() const {
    auto result = SendCommand("INFO");
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    // Parse Redis-style response (key: value lines)
    ServerInfo info;
    std::istringstream iss(*result);
    std::string line;

    while (std::getline(iss, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }

      size_t colon_pos = line.find(':');
      if (colon_pos == std::string::npos) {
        continue;
      }

      std::string key = line.substr(0, colon_pos);
      std::string value = line.substr(colon_pos + 1);

      // Trim whitespace
      value.erase(0, value.find_first_not_of(" \t\r\n"));
      value.erase(value.find_last_not_of(" \t\r\n") + 1);

      // Keys mirror the server's INFO output (info_handler.cpp). Values are
      // parsed without throwing so malformed input cannot crash the client.
      if (key == "version") {
        info.version = value;
      } else if (key == "uptime_seconds") {
        info.uptime_seconds = ParseUint64(value);
      } else if (key == "total_commands_processed") {
        info.total_commands_processed = ParseUint64(value);
      } else if (key == "failed_commands") {
        info.failed_commands = ParseUint64(value);
      } else if (key == "total_connections_received") {
        info.total_connections = ParseUint64(value);
      } else if (key == "active_connections") {
        info.active_connections = ParseUint64(value);
      } else if (key == "event_count") {
        info.event_count = ParseUint64(value);
      } else if (key == "vector_count") {
        info.vector_count = ParseUint64(value);
      } else if (key == "id_count") {
        info.id_count = ParseUint64(value);
      } else if (key == "ctx_count") {
        info.ctx_count = ParseUint64(value);
      }
    }

    return info;
  }

  Expected<std::string, Error> GetConfig() const {
    auto result = SendCommand("CONFIG SHOW");
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    return *result;
  }

  Expected<std::string, Error> Save(const std::string& filepath) const {
    std::ostringstream cmd;
    if (filepath.empty()) {
      cmd << "DUMP SAVE";
    } else {
      if (auto err = ValidateProtocolToken(filepath, "snapshot filepath")) {
        return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
      }
      cmd << "DUMP SAVE " << filepath;
    }

    auto result = SendCommand(cmd.str());
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    constexpr std::string_view kSavedPrefix = "OK DUMP_SAVED ";
    constexpr std::string_view kSaveStartedPrefix = "OK DUMP_SAVE_STARTED ";
    const std::string_view prefix = result->rfind(kSavedPrefix, 0) == 0 ? kSavedPrefix : kSaveStartedPrefix;
    if (result->rfind(prefix, 0) == 0) {
      std::string path = result->substr(prefix.size());
      path.erase(path.find_last_not_of("\r\n") + 1);
      return path;
    }

    return MakeUnexpected(MakeError(ErrorCode::kClientProtocolError, "Unexpected DUMP SAVE response: " + *result));
  }

  Expected<std::string, Error> Load(const std::string& filepath) const {
    if (filepath.empty()) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Filepath cannot be empty for LOAD"));
    }

    if (auto err = ValidateProtocolToken(filepath, "snapshot filepath")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    std::ostringstream cmd;
    cmd << "DUMP LOAD " << filepath;

    auto result = SendCommand(cmd.str());
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    constexpr std::string_view kLoadedPrefix = "OK DUMP_LOADED ";
    if (result->rfind(kLoadedPrefix, 0) == 0) {
      std::string path = result->substr(kLoadedPrefix.size());
      path.erase(path.find_last_not_of("\r\n") + 1);
      return path;
    }

    return MakeUnexpected(MakeError(ErrorCode::kClientProtocolError, "Unexpected DUMP LOAD response: " + *result));
  }

  Expected<std::string, Error> Verify(const std::string& filepath) const {
    if (filepath.empty()) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Filepath cannot be empty for VERIFY"));
    }

    if (auto err = ValidateProtocolToken(filepath, "snapshot filepath")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    std::ostringstream cmd;
    cmd << "DUMP VERIFY " << filepath;

    auto result = SendCommand(cmd.str());
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    return *result;
  }

  Expected<std::string, Error> DumpInfo(const std::string& filepath) const {
    if (filepath.empty()) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Filepath cannot be empty for DUMP INFO"));
    }

    if (auto err = ValidateProtocolToken(filepath, "snapshot filepath")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    std::ostringstream cmd;
    cmd << "DUMP INFO " << filepath;

    auto result = SendCommand(cmd.str());
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    return *result;
  }

  Expected<void, Error> EnableDebug() const {
    auto result = SendCommand("DEBUG ON");
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    if (result->find("OK") != 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientProtocolError, "Unexpected response: " + *result));
    }

    return {};
  }

  Expected<void, Error> DisableDebug() const {
    auto result = SendCommand("DEBUG OFF");
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    if (result->find("OK") != 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientProtocolError, "Unexpected response: " + *result));
    }

    return {};
  }

  Expected<void, Error> Auth(const std::string& password) const {
    if (auto err = ValidateNoControlCharacters(password, "password")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }

    std::ostringstream cmd;
    // AUTH is the sole command whose argument is an opaque suffix rather than
    // a token. The server preserves all bytes after this delimiter, including
    // leading, trailing, and repeated spaces; quoting here would turn those
    // bytes into part of the password.
    cmd << "AUTH " << password;

    auto result = SendCommand(cmd.str());
    if (!result) {
      return MakeUnexpected(result.error());
    }

    return CheckOkResponse(*result);
  }

  Expected<std::string, Error> DumpStatus() const {
    auto result = SendCommand("DUMP STATUS");
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (IsErrorResponse(*result)) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, ErrorBody(*result)));
    }

    return *result;
  }

  Expected<std::string, Error> CacheStats() const {
    auto result = SendCommand("CACHE STATS");
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (IsErrorResponse(*result)) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, ErrorBody(*result)));
    }

    return *result;
  }

  Expected<void, Error> CacheClear() const { return SimpleCacheCommand("CACHE CLEAR"); }

  Expected<void, Error> CacheEnable() const { return SimpleCacheCommand("CACHE ENABLE"); }

  Expected<void, Error> CacheDisable() const { return SimpleCacheCommand("CACHE DISABLE"); }

 private:
  // Whether a response is an error, accepting both "ERROR ..." and Redis-style
  // "-ERR ..." / "(error) ..." prefixes the server may emit.
  static bool IsErrorResponse(const std::string& response) {
    return response.find("ERROR") == 0 || response.find("-ERR") == 0 || response.find("(error)") == 0;
  }

  // Extract the human-readable body of an error response for the Error message.
  static std::string ErrorBody(const std::string& response) {
    if (response.find("ERROR ") == 0) {
      return response.substr(kErrorPrefixLen);
    }
    return response;
  }

  // Validate a generic success response, accepting both "OK ..." and the
  // Redis-style "+OK ..." prefix (AUTH returns "+OK").
  static Expected<void, Error> CheckOkResponse(const std::string& response) {
    if (IsErrorResponse(response)) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, ErrorBody(response)));
    }
    if (response.find("OK") != 0 && response.find("+OK") != 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientProtocolError, "Unexpected response: " + response));
    }
    return {};
  }

  // Issue a CACHE command whose success response is a single "OK ..." line.
  Expected<void, Error> SimpleCacheCommand(const std::string& command) const {
    auto result = SendCommand(command);
    if (!result) {
      return MakeUnexpected(result.error());
    }
    return CheckOkResponse(*result);
  }

  ClientConfig config_;
  mutable std::mutex round_trip_mutex_;
  mutable int sock_ = -1;
  mutable std::string response_buffer_;
  mutable bool debug_mode_ = false;
};

//
// NvecdClient public interface implementation
//

NvecdClient::NvecdClient(ClientConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

NvecdClient::~NvecdClient() = default;

NvecdClient::NvecdClient(NvecdClient&&) noexcept = default;

NvecdClient& NvecdClient::operator=(NvecdClient&&) noexcept = default;

Expected<void, Error> NvecdClient::Connect() {
  return impl_->Connect();
}

void NvecdClient::Disconnect() {
  impl_->Disconnect();
}

bool NvecdClient::IsConnected() const {
  return impl_->IsConnected();
}

Expected<void, Error> NvecdClient::Event(const std::string& ctx, const std::string& type, const std::string& id,
                                         int score) const {
  return impl_->Event(ctx, type, id, score);
}

Expected<void, Error> NvecdClient::Vecset(const std::string& id, const std::vector<float>& vector) const {
  return impl_->Vecset(id, vector);
}

Expected<void, Error> NvecdClient::Vecdel(const std::string& id) const {
  return impl_->Vecdel(id);
}

Expected<void, Error> NvecdClient::Metaset(const std::string& id, const std::string& metadata) const {
  return impl_->Metaset(id, metadata);
}

Expected<SimResponse, Error> NvecdClient::Sim(const std::string& id, uint32_t top_k, const std::string& mode,
                                              const SearchOptions& options) const {
  return impl_->Sim(id, top_k, mode, options);
}

Expected<SimResponse, Error> NvecdClient::Simv(const std::vector<float>& vector, uint32_t top_k,
                                               const std::string& mode, const SearchOptions& options) const {
  return impl_->Simv(vector, top_k, mode, options);
}

Expected<void, Error> NvecdClient::Auth(const std::string& password) const {
  return impl_->Auth(password);
}

Expected<ServerInfo, Error> NvecdClient::Info() const {
  return impl_->Info();
}

Expected<std::string, Error> NvecdClient::GetConfig() const {
  return impl_->GetConfig();
}

Expected<std::string, Error> NvecdClient::Save(const std::string& filepath) const {
  return impl_->Save(filepath);
}

Expected<std::string, Error> NvecdClient::Load(const std::string& filepath) const {
  return impl_->Load(filepath);
}

Expected<std::string, Error> NvecdClient::Verify(const std::string& filepath) const {
  return impl_->Verify(filepath);
}

Expected<std::string, Error> NvecdClient::DumpInfo(const std::string& filepath) const {
  return impl_->DumpInfo(filepath);
}

Expected<std::string, Error> NvecdClient::DumpStatus() const {
  return impl_->DumpStatus();
}

Expected<std::string, Error> NvecdClient::CacheStats() const {
  return impl_->CacheStats();
}

Expected<void, Error> NvecdClient::CacheClear() const {
  return impl_->CacheClear();
}

Expected<void, Error> NvecdClient::CacheEnable() const {
  return impl_->CacheEnable();
}

Expected<void, Error> NvecdClient::CacheDisable() const {
  return impl_->CacheDisable();
}

Expected<void, Error> NvecdClient::EnableDebug() const {
  return impl_->EnableDebug();
}

Expected<void, Error> NvecdClient::DisableDebug() const {
  return impl_->DisableDebug();
}

Expected<std::string, Error> NvecdClient::SendCommand(const std::string& command) const {
  return impl_->SendCommand(command);
}

}  // namespace nvecd::client

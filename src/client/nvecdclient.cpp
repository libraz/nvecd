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
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>

#include "utils/error.h"
#include "utils/expected.h"

using namespace nvecd::utils;

namespace nvecd::client {

namespace {

// Protocol constants
constexpr size_t kErrorPrefixLen = 6;  // Length of "ERROR "
constexpr int kMillisecondsPerSecond = 1000;
constexpr int kMicrosecondsPerMillisecond = 1000;

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
 * @brief Escape special characters in strings
 */
std::string EscapeString(const std::string& str) {
  // Check if string needs quoting (contains spaces or special chars)
  bool needs_quotes = false;
  for (char character : str) {
    if (character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '"' ||
        character == '\'') {
      needs_quotes = true;
      break;
    }
  }

  if (!needs_quotes) {
    return str;
  }

  // Use double quotes and escape internal quotes
  std::string result = "\"";
  for (char character : str) {
    if (character == '"' || character == '\\') {
      result += '\\';
    }
    result += character;
  }
  result += '"';
  return result;
}

}  // namespace

/**
 * @brief PIMPL implementation class
 */
class NvecdClient::Impl {
 public:
  explicit Impl(ClientConfig config) : config_(std::move(config)) {}

  ~Impl() { Disconnect(); }

  // Non-copyable, movable
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = default;
  Impl& operator=(Impl&&) = default;

  Expected<void, Error> Connect() {
    if (sock_ >= 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientAlreadyConnected, "Already connected"));
    }

    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
      return MakeUnexpected(
          MakeError(ErrorCode::kClientConnectionFailed, std::string("Failed to create socket: ") + strerror(errno)));
    }

    // Set socket timeout
    struct timeval timeout_val = {};
    timeout_val.tv_sec = static_cast<decltype(timeout_val.tv_sec)>(config_.timeout_ms / kMillisecondsPerSecond);
    timeout_val.tv_usec = static_cast<decltype(timeout_val.tv_usec)>((config_.timeout_ms % kMillisecondsPerSecond) *
                                                                     kMicrosecondsPerMillisecond);
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &timeout_val, sizeof(timeout_val));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &timeout_val, sizeof(timeout_val));

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

  void Disconnect() {
    if (sock_ >= 0) {
      close(sock_);
      sock_ = -1;
    }
  }

  [[nodiscard]] bool IsConnected() const { return sock_ >= 0; }

  Expected<std::string, Error> SendCommand(const std::string& command) const {
    if (!IsConnected()) {
      return MakeUnexpected(MakeError(ErrorCode::kClientNotConnected, "Not connected"));
    }

    // Send command with \r\n terminator
    std::string msg = command + "\r\n";
    ssize_t sent = send(sock_, msg.c_str(), msg.length(), 0);
    if (sent < 0) {
      return MakeUnexpected(
          MakeError(ErrorCode::kClientCommandFailed, std::string("Failed to send command: ") + strerror(errno)));
    }

    // Receive response (loop until complete response is received)
    std::string response;
    std::vector<char> buffer(config_.recv_buffer_size);

    while (true) {
      ssize_t received = recv(sock_, buffer.data(), buffer.size() - 1, 0);
      if (received <= 0) {
        if (received == 0) {
          return MakeUnexpected(MakeError(ErrorCode::kClientConnectionClosed, "Connection closed by server"));
        }
        return MakeUnexpected(
            MakeError(ErrorCode::kClientCommandFailed, std::string("Failed to receive response: ") + strerror(errno)));
      }

      buffer[received] = '\0';
      response.append(buffer.data(), received);

      // Check if response is complete by looking for \r\n terminator
      if (response.size() >= 2 && response[response.size() - 2] == '\r' && response[response.size() - 1] == '\n') {
        break;
      }

      // If we received less than buffer size, server has no more data
      if (static_cast<size_t>(received) < buffer.size() - 1) {
        // Small delay to allow more data to arrive
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    // Remove trailing \r\n
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r')) {
      response.pop_back();
    }

    return response;
  }

  //
  // nvecd-specific commands
  //

  Expected<void, Error> Event(const std::string& ctx, const std::string& type, const std::string& id, int score) const {
    if (auto err = ValidateNoControlCharacters(ctx, "context ID")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (auto err = ValidateNoControlCharacters(type, "event type")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (auto err = ValidateNoControlCharacters(id, "document ID")) {
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
    cmd << "EVENT " << EscapeString(ctx) << " " << type << " " << EscapeString(id);
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
    if (auto err = ValidateNoControlCharacters(id, "vector ID")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (vector.empty()) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Vector cannot be empty"));
    }

    std::ostringstream cmd;
    cmd << std::fixed;  // Use fixed-point notation
    cmd << "VECSET " << EscapeString(id);
    for (float val : vector) {
      cmd << " " << val;
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

  Expected<SimResponse, Error> Sim(const std::string& id, uint32_t top_k, const std::string& mode) const {
    if (auto err = ValidateNoControlCharacters(id, "document ID")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }
    if (auto err = ValidateNoControlCharacters(mode, "search mode")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }

    std::ostringstream cmd;
    cmd << "SIM " << EscapeString(id) << " " << top_k;
    if (!mode.empty() && mode != "fusion") {
      cmd << " using=" << mode;
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

  Expected<SimResponse, Error> Simv(const std::vector<float>& vector, uint32_t top_k, const std::string& mode) const {
    if (vector.empty()) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Vector cannot be empty"));
    }
    if (auto err = ValidateNoControlCharacters(mode, "search mode")) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, *err));
    }

    std::ostringstream cmd;
    cmd << std::fixed;  // Use fixed-point notation
    cmd << "SIMV " << top_k;
    for (float val : vector) {
      cmd << " " << val;
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

      if (key == "version") {
        info.version = value;
      } else if (key == "uptime_seconds") {
        info.uptime_seconds = std::stoull(value);
      } else if (key == "total_requests") {
        info.total_requests = std::stoull(value);
      } else if (key == "active_connections") {
        info.active_connections = std::stoull(value);
      } else if (key == "event_count") {
        info.event_count = std::stoull(value);
      } else if (key == "vector_count") {
        info.vector_count = std::stoull(value);
      } else if (key == "co_occurrence_entries") {
        info.co_occurrence_entries = std::stoull(value);
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
      cmd << "DUMP SAVE " << EscapeString(filepath);
    }

    auto result = SendCommand(cmd.str());
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    // Response format: "SNAPSHOT <filepath>"
    if (result->find("SNAPSHOT ") == 0) {
      return result->substr(9);  // NOLINT(readability-magic-numbers) - Length of "SNAPSHOT "
    }

    return *result;
  }

  Expected<std::string, Error> Load(const std::string& filepath) const {
    if (filepath.empty()) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Filepath cannot be empty for LOAD"));
    }

    std::ostringstream cmd;
    cmd << "DUMP LOAD " << EscapeString(filepath);

    auto result = SendCommand(cmd.str());
    if (!result) {
      return MakeUnexpected(result.error());
    }

    if (result->find("ERROR") == 0) {
      return MakeUnexpected(MakeError(ErrorCode::kClientServerError, result->substr(kErrorPrefixLen)));
    }

    // Response format: "SNAPSHOT: <filepath>"
    if (result->find("SNAPSHOT: ") == 0) {
      return result->substr(10);  // NOLINT(readability-magic-numbers) - Length of "SNAPSHOT: "
    }

    return *result;
  }

  Expected<std::string, Error> Verify(const std::string& filepath) const {
    if (filepath.empty()) {
      return MakeUnexpected(MakeError(ErrorCode::kClientInvalidArgument, "Filepath cannot be empty for VERIFY"));
    }

    std::ostringstream cmd;
    cmd << "DUMP VERIFY " << EscapeString(filepath);

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

    std::ostringstream cmd;
    cmd << "DUMP INFO " << EscapeString(filepath);

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

 private:
  ClientConfig config_;
  mutable int sock_ = -1;
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

Expected<SimResponse, Error> NvecdClient::Sim(const std::string& id, uint32_t top_k, const std::string& mode) const {
  return impl_->Sim(id, top_k, mode);
}

Expected<SimResponse, Error> NvecdClient::Simv(const std::vector<float>& vector, uint32_t top_k,
                                               const std::string& mode) const {
  return impl_->Simv(vector, top_k, mode);
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

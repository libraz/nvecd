/**
 * @file command_parser.cpp
 * @brief Command parser implementation
 *
 * Reference: ../mygram-db/src/query/query_parser.cpp
 * Reusability: 40% (parsing utilities reused, command syntax different)
 */

#include "server/command_parser.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "utils/structured_log.h"

namespace nvecd::server {

namespace {

/**
 * @brief Split string by delimiter
 */
std::vector<std::string> Split(const std::string& str, char delimiter) {
  std::vector<std::string> tokens;
  std::stringstream ss(str);
  std::string token;
  while (std::getline(ss, token, delimiter)) {
    if (!token.empty()) {
      tokens.push_back(token);
    }
  }
  return tokens;
}

/**
 * @brief Trim whitespace from both ends
 */
std::string Trim(const std::string& str) {
  auto start = std::find_if_not(str.begin(), str.end(), [](unsigned char ch) { return std::isspace(ch); });
  auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char ch) { return std::isspace(ch); }).base();
  return (start < end) ? std::string(start, end) : "";
}

/**
 * @brief Convert string to uppercase
 */
std::string ToUpper(const std::string& str) {
  std::string result = str;
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::toupper(c); });
  return result;
}

/**
 * @brief Parse integer from string
 */
utils::Expected<int, utils::Error> ParseInt(const std::string& str) {
  try {
    size_t pos = 0;
    int value = std::stoi(str, &pos);
    if (pos != str.length()) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Invalid integer: " + str));
    }
    return value;
  } catch (const std::exception& e) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Failed to parse integer: " + str));
  }
}

/**
 * @brief Parse float from string
 */
utils::Expected<float, utils::Error> ParseFloat(const std::string& str) {
  try {
    size_t pos = 0;
    float value = std::stof(str, &pos);
    if (pos != str.length()) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Invalid float: " + str));
    }
    return value;
  } catch (const std::exception& e) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Failed to parse float: " + str));
  }
}

}  // namespace

utils::Expected<std::vector<float>, utils::Error> ParseVector(const std::string& vec_str, int expected_dim) {
  std::vector<float> vec;
  std::stringstream ss(vec_str);
  float value;

  while (ss >> value) {
    vec.push_back(value);
  }

  if (vec.empty()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandInvalidVector, "Empty vector"));
  }

  if (expected_dim > 0 && static_cast<int>(vec.size()) != expected_dim) {
    return utils::MakeUnexpected(utils::MakeError(
        utils::ErrorCode::kCommandInvalidVector,
        "Vector dimension mismatch: expected " + std::to_string(expected_dim) + ", got " + std::to_string(vec.size())));
  }

  return vec;
}

utils::Expected<Command, utils::Error> ParseCommand(const std::string& request) {
  if (request.empty()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "Empty command"));
  }

  // Split by newlines for multi-line commands (VECSET, SIMV)
  auto lines = Split(request, '\n');
  if (lines.empty()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "Empty command"));
  }

  // Parse first line (command and args)
  std::string first_line = Trim(lines[0]);
  auto tokens = Split(first_line, ' ');
  if (tokens.empty()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "Empty command"));
  }

  Command cmd;
  std::string cmd_name = ToUpper(tokens[0]);

  // Parse command type and arguments
  if (cmd_name == "EVENT") {
    // EVENT <ctx> <id> <score>
    if (tokens.size() != 4) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "EVENT requires 3 arguments: <ctx> <id> <score>"));
    }
    cmd.type = CommandType::kEvent;
    cmd.ctx = tokens[1];
    cmd.id = tokens[2];
    auto score_result = ParseInt(tokens[3]);
    if (!score_result) {
      return utils::MakeUnexpected(score_result.error());
    }
    cmd.score = *score_result;

  } else if (cmd_name == "VECSET") {
    // VECSET <id> <f1> <f2> ... <fN>
    if (tokens.size() < 3) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "VECSET requires at least 2 arguments: <id> <floats>"));
    }
    cmd.type = CommandType::kVecset;
    cmd.id = tokens[1];

    // Parse vector from remaining tokens
    std::vector<float> vec;
    vec.reserve(tokens.size() - 2);
    for (size_t i = 2; i < tokens.size(); ++i) {
      auto val_result = ParseFloat(tokens[i]);
      if (!val_result) {
        return utils::MakeUnexpected(val_result.error());
      }
      vec.push_back(*val_result);
    }

    cmd.dimension = static_cast<int>(vec.size());
    cmd.vector = std::move(vec);

  } else if (cmd_name == "SIM") {
    // SIM <id> <top_k> [using=mode]
    if (tokens.size() < 3) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "SIM requires at least 2 arguments: <id> <top_k>"));
    }
    cmd.type = CommandType::kSim;
    cmd.id = tokens[1];

    auto top_k_result = ParseInt(tokens[2]);
    if (!top_k_result) {
      return utils::MakeUnexpected(top_k_result.error());
    }
    cmd.top_k = *top_k_result;

    // Parse optional using=mode
    if (tokens.size() >= 4) {
      std::string mode_arg = tokens[3];
      if (mode_arg.rfind("using=", 0) == 0) {
        cmd.mode = mode_arg.substr(6);  // Skip "using="
      } else {
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "Invalid SIM option: " + mode_arg));
      }
    }

  } else if (cmd_name == "SIMV") {
    // SIMV <top_k> <f1> <f2> ... <fN>
    if (tokens.size() < 3) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "SIMV requires at least 2 arguments: <top_k> <floats>"));
    }
    cmd.type = CommandType::kSimv;

    auto top_k_result = ParseInt(tokens[1]);
    if (!top_k_result) {
      return utils::MakeUnexpected(top_k_result.error());
    }
    cmd.top_k = *top_k_result;

    // Parse vector from remaining tokens
    std::vector<float> vec;
    vec.reserve(tokens.size() - 2);
    for (size_t i = 2; i < tokens.size(); ++i) {
      auto val_result = ParseFloat(tokens[i]);
      if (!val_result) {
        return utils::MakeUnexpected(val_result.error());
      }
      vec.push_back(*val_result);
    }

    cmd.dimension = static_cast<int>(vec.size());
    cmd.vector = std::move(vec);

  } else if (cmd_name == "INFO") {
    cmd.type = CommandType::kInfo;

  } else if (cmd_name == "CONFIG") {
    // CONFIG HELP|SHOW|VERIFY [path]
    if (tokens.size() < 2) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "CONFIG requires subcommand: HELP|SHOW|VERIFY"));
    }
    std::string subcmd = ToUpper(tokens[1]);
    if (subcmd == "HELP") {
      cmd.type = CommandType::kConfigHelp;
    } else if (subcmd == "SHOW") {
      cmd.type = CommandType::kConfigShow;
    } else if (subcmd == "VERIFY") {
      cmd.type = CommandType::kConfigVerify;
    } else {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "Unknown CONFIG subcommand: " + subcmd));
    }
    if (tokens.size() >= 3) {
      cmd.path = tokens[2];
    }

  } else if (cmd_name == "DUMP") {
    // DUMP SAVE|LOAD|VERIFY|INFO [filepath]
    if (tokens.size() < 2) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "DUMP requires subcommand: SAVE|LOAD|VERIFY|INFO"));
    }
    std::string subcmd = ToUpper(tokens[1]);
    if (subcmd == "SAVE") {
      cmd.type = CommandType::kDumpSave;
    } else if (subcmd == "LOAD") {
      cmd.type = CommandType::kDumpLoad;
    } else if (subcmd == "VERIFY") {
      cmd.type = CommandType::kDumpVerify;
    } else if (subcmd == "INFO") {
      cmd.type = CommandType::kDumpInfo;
    } else {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "Unknown DUMP subcommand: " + subcmd));
    }
    if (tokens.size() >= 3) {
      cmd.path = tokens[2];
    }

  } else if (cmd_name == "DEBUG") {
    // DEBUG ON|OFF
    if (tokens.size() < 2) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "DEBUG requires argument: ON|OFF"));
    }
    std::string arg = ToUpper(tokens[1]);
    if (arg == "ON") {
      cmd.type = CommandType::kDebugOn;
    } else if (arg == "OFF") {
      cmd.type = CommandType::kDebugOff;
    } else {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "DEBUG requires ON or OFF, got: " + arg));
    }

  } else if (cmd_name == "CACHE") {
    // CACHE STATS|CLEAR|ENABLE|DISABLE
    if (tokens.size() < 2) {
      return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCommandSyntaxError,
                                                     "CACHE requires subcommand: STATS|CLEAR|ENABLE|DISABLE"));
    }
    std::string subcommand = ToUpper(tokens[1]);
    if (subcommand == "STATS") {
      cmd.type = CommandType::kCacheStats;
    } else if (subcommand == "CLEAR") {
      cmd.type = CommandType::kCacheClear;
    } else if (subcommand == "ENABLE") {
      cmd.type = CommandType::kCacheEnable;
    } else if (subcommand == "DISABLE") {
      cmd.type = CommandType::kCacheDisable;
    } else {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandSyntaxError, "Unknown CACHE subcommand: " + subcommand));
    }

  } else {
    cmd.type = CommandType::kUnknown;
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandUnknown, "Unknown command: " + cmd_name));
  }

  return cmd;
}

}  // namespace nvecd::server

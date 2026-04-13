/**
 * @file command_parser.h
 * @brief Command parser for nvecd protocol
 *
 * Reference: ../mygram-db/src/query/query_parser.h
 * Reusability: 40% (similar parsing pattern, different syntax)
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "events/event_store.h"
// nameser.h (included via httplib.h on Linux) may define ADD as a macro
// after event_store.h was already processed via #pragma once.
#ifdef ADD
#undef ADD
#endif
#include "server/command_types.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server {

/**
 * @brief Parsed command structure
 *
 * Contains all possible fields for different command types.
 * Only relevant fields are populated based on command type.
 */
struct Command {
  CommandType type = CommandType::kUnknown;

  // EVENT fields
  std::string ctx;                                        // Context ID
  std::string id;                                         // Item ID (EVENT, SIM, VECSET)
  int score = 0;                                          // Event score (EVENT)
  events::EventType event_type = events::EventType::ADD;  // Event type (ADD/SET/DEL)

  // SIM/SIMV fields
  int top_k = 100;  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) Number of results
  std::string mode = "fusion";        // Similarity mode: events, vectors, fusion
  std::optional<uint64_t> timestamp;  // Optional timestamp for EVENT (epoch seconds)
  std::optional<bool> adaptive;       // Optional adaptive flag for SIM
  std::string filter_expr;            // Filter expression (e.g., "status:active,type:news")
  float min_score = 0.0F;             // Minimum score threshold
  uint32_t candidate_limit = 0;       // Candidate limit for post-filter (0 = auto)
  bool explain = false;               // Show score breakdown

  // VECSET/SIMV fields
  int dimension = 0;          // Vector dimension
  std::vector<float> vector;  // Vector data

  // CONFIG/DUMP fields
  std::string path;  // Config path or dump filepath

  // SET/GET/SHOW VARIABLES fields
  std::string variable_name;   // Variable name (e.g., "logging.level")
  std::string variable_value;  // Variable value for SET
  std::string pattern;         // Pattern for SHOW VARIABLES LIKE
};

/**
 * @brief Parse a command from request string
 *
 * Parses text-based protocol commands:
 * - EVENT <ctx> ADD <id> <score> [timestamp=<epoch_sec>]
 * - EVENT <ctx> SET <id> <score> [timestamp=<epoch_sec>]
 * - EVENT <ctx> DEL <id> [timestamp=<epoch_sec>]
 * - VECSET <id> <dim> text\n<floats>
 * - SIM <id> <top_k> [using=mode] [adaptive=on|off]
 * - SIMV <dim> <top_k>\n<floats>
 * - INFO
 * - CONFIG HELP|SHOW|VERIFY [path]
 * - DUMP SAVE|LOAD|VERIFY|INFO [filepath]
 * - DEBUG ON|OFF
 *
 * @param request Raw request string (may contain multiple lines for VECSET/SIMV)
 * @return Expected<Command, Error> Parsed command or error
 */
utils::Expected<Command, utils::Error> ParseCommand(const std::string& request);

/**
 * @brief Parse a vector from string (space-separated floats)
 *
 * @param vec_str String containing space-separated floats
 * @param expected_dim Expected dimension (0 = any dimension)
 * @return Expected<vector<float>, Error> Parsed vector or error
 */
utils::Expected<std::vector<float>, utils::Error> ParseVector(const std::string& vec_str, int expected_dim = 0);

}  // namespace nvecd::server

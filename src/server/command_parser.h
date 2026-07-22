/**
 * @file command_parser.h
 * @brief Command parser for nvecd protocol
 *
 * Reference: ../mygram-db/src/query/query_parser.h
 * Reusability: 40% (similar parsing pattern, different syntax)
 */

#pragma once

#include <cstdint>
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
#include "vectors/metadata.h"

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
  std::string id;                                         // Item ID (EVENT, SIM, VECSET, VECDEL)
  int score = 0;                                          // Event score (EVENT)
  events::EventType event_type = events::EventType::ADD;  // Event type (ADD/SET/DEL)

  // SIM/SIMV fields
  int top_k = 100;  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) Number of results
  std::string mode = "fusion";        // Similarity mode: events, vectors, fusion
  std::optional<uint64_t> timestamp;  // Optional timestamp for EVENT (epoch seconds)
  std::optional<bool> adaptive;       // Optional adaptive flag for SIM
  std::string filter_expr;            // Filter or metadata expression (e.g., "status:active,type:news")
  // Typed metadata is used by HTTP WAL records. TCP METASET continues to use
  // filter_expr so its text protocol remains unchanged.
  std::optional<vectors::Metadata> metadata;
  float min_score = 0.0F;  // Minimum score threshold

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
 * - VECSET <id> <f1> <f2> ... <fN>
 * - VECDEL <id>
 * - METASET <id> <key:value[,key:value...]>
 * - SIM <id> <top_k> [using=mode] [adaptive=on|off]
 * - SIMV <top_k> [filter=<expr>] [min_score=<float>] <f1> <f2> ... <fN>
 * - INFO
 * - CONFIG HELP|SHOW|VERIFY [path]
 * - DUMP SAVE|LOAD|VERIFY|INFO [filepath]
 * - DEBUG ON|OFF
 *
 * @param request Raw single-line request string (an optional final CRLF is accepted)
 * @param max_top_k Maximum allowed top_k for SIM/SIMV (0 = no upper-bound check)
 * @return Expected<Command, Error> Parsed command or error
 */
utils::Expected<Command, utils::Error> ParseCommand(const std::string& request, uint32_t max_top_k = 0);

/**
 * @brief Parse a vector from string (space-separated floats)
 *
 * @param vec_str String containing space-separated floats
 * @param expected_dim Expected dimension (0 = any dimension)
 * @return Expected<vector<float>, Error> Parsed vector or error
 */
utils::Expected<std::vector<float>, utils::Error> ParseVector(const std::string& vec_str, int expected_dim = 0);

}  // namespace nvecd::server

/**
 * @file filter_parser.cpp
 * @brief Simple filter expression parser implementation
 */

#include "server/filter_parser.h"

#include <cerrno>
#include <cstdlib>

namespace nvecd::server {

namespace {

/// Try to parse a value string as the most specific type possible
vectors::MetadataValue ParseValue(const std::string& val) {
  // Try bool
  if (val == "true") {
    return true;
  }
  if (val == "false") {
    return false;
  }

  // Try int64
  {
    char* end = nullptr;
    errno = 0;
    long long int_val = std::strtoll(val.c_str(), &end, 10);  // NOLINT(runtime/int)
    if (end != val.c_str() && *end == '\0' && errno == 0) {
      return static_cast<int64_t>(int_val);
    }
  }

  // Try double
  {
    char* end = nullptr;
    errno = 0;
    double dbl_val = std::strtod(val.c_str(), &end);
    if (end != val.c_str() && *end == '\0' && errno == 0) {
      return dbl_val;
    }
  }

  // Default: string
  return val;
}

}  // namespace

utils::Expected<vectors::MetadataFilter, utils::Error> ParseSimpleFilter(
    const std::string& expr) {
  vectors::MetadataFilter filter;

  if (expr.empty()) {
    return filter;
  }

  // Split by ','
  size_t pos = 0;
  while (pos < expr.size()) {
    size_t comma = expr.find(',', pos);
    if (comma == std::string::npos) {
      comma = expr.size();
    }

    std::string pair = expr.substr(pos, comma - pos);
    pos = comma + 1;

    if (pair.empty()) {
      continue;
    }

    // Split by first ':'
    size_t colon = pair.find(':');
    if (colon == std::string::npos || colon == 0) {
      return utils::MakeUnexpected(utils::MakeError(
          utils::ErrorCode::kCommandParseError,
          "Invalid filter condition: missing ':' in '" + pair + "'"));
    }

    std::string key = pair.substr(0, colon);
    std::string val = pair.substr(colon + 1);

    if (val.empty()) {
      return utils::MakeUnexpected(utils::MakeError(
          utils::ErrorCode::kCommandParseError,
          "Empty value for filter key '" + key + "'"));
    }

    vectors::FilterCondition cond;
    cond.field = key;
    cond.op = vectors::FilterOp::kEq;
    cond.value = ParseValue(val);
    filter.conditions.push_back(std::move(cond));
  }

  return filter;
}

}  // namespace nvecd::server

/**
 * @file filter_parser.cpp
 * @brief Simple filter expression parser implementation
 */

#include "server/filter_parser.h"

#include <cerrno>
#include <cstdlib>
#include <string_view>

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

bool ParseCondition(const std::string& pair, vectors::FilterCondition* condition) {
  struct Operator {
    std::string_view text;
    vectors::FilterOp op;
  };
  constexpr Operator kOperators[] = {{"!=", vectors::FilterOp::kNe}, {">=", vectors::FilterOp::kGe},
                                     {"<=", vectors::FilterOp::kLe}, {"=", vectors::FilterOp::kEq},
                                     {">", vectors::FilterOp::kGt},  {"<", vectors::FilterOp::kLt},
                                     {":", vectors::FilterOp::kEq}};

  size_t operator_pos = std::string::npos;
  const Operator* selected = nullptr;
  for (const auto& candidate : kOperators) {
    const size_t pos = pair.find(candidate.text);
    if (pos != std::string::npos && (operator_pos == std::string::npos || pos < operator_pos ||
                                     (pos == operator_pos && candidate.text.size() > selected->text.size()))) {
      operator_pos = pos;
      selected = &candidate;
    }
  }
  if (selected == nullptr || operator_pos == 0) {
    return false;
  }

  condition->field = pair.substr(0, operator_pos);
  const std::string value = pair.substr(operator_pos + selected->text.size());
  if (value.empty()) {
    return false;
  }

  condition->op = selected->op;
  if (selected->op == vectors::FilterOp::kEq && value.size() >= 5 && value.rfind("in(", 0) == 0 &&
      value.back() == ')') {
    condition->op = vectors::FilterOp::kIn;
    const std::string list = value.substr(3, value.size() - 4);
    size_t start = 0;
    while (start <= list.size()) {
      const size_t separator = list.find('|', start);
      const std::string item =
          list.substr(start, separator == std::string::npos ? std::string::npos : separator - start);
      if (item.empty()) {
        return false;
      }
      condition->values.push_back(ParseValue(item));
      if (separator == std::string::npos) {
        break;
      }
      start = separator + 1;
    }
    return !condition->values.empty();
  }

  condition->value = ParseValue(value);
  return true;
}

}  // namespace

utils::Expected<vectors::MetadataFilter, utils::Error> ParseSimpleFilter(const std::string& expr) {
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

    vectors::FilterCondition cond;
    if (!ParseCondition(pair, &cond)) {
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kCommandParseError, "Invalid filter condition: '" + pair + "'"));
    }
    filter.conditions.push_back(std::move(cond));
  }

  return filter;
}

}  // namespace nvecd::server

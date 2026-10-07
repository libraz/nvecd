/**
 * @file filter_parser.cpp
 * @brief Simple filter expression parser implementation
 */

#include "server/filter_parser.h"

#include <string_view>

#include "server/argument_validation.h"

namespace nvecd::server {

namespace {

/// Type a value by its spelling: true/false, then a decimal integer, then a
/// finite decimal literal; anything else (nan, inf, hex) stays a string.
vectors::MetadataValue ParseValue(const std::string& val) {
  if (val == "true") {
    return true;
  }
  if (val == "false") {
    return false;
  }
  if (auto integer = ParseDecimalInteger(val)) {
    return *integer;
  }
  if (auto number = ParseDecimalNumber(val)) {
    return *number;
  }
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

utils::Expected<vectors::Metadata, utils::Error> ParseMetadataPairs(const std::string& expr) {
  auto parsed = ParseSimpleFilter(expr);
  if (!parsed) {
    return utils::MakeUnexpected(parsed.error());
  }
  if (parsed->conditions.empty()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "METASET requires at least one key:value pair"));
  }
  vectors::Metadata metadata;
  for (auto& condition : parsed->conditions) {
    // A stored value is a single value: comparisons and in(...) lists have no
    // metadata meaning, so they are rejected rather than reduced to one.
    if (condition.op != vectors::FilterOp::kEq) {
      return utils::MakeUnexpected(utils::MakeError(
          utils::ErrorCode::kCommandInvalidArgument,
          "METASET accepts only key:value pairs, got '" + condition.field + "' with a comparison or in(...) value"));
    }
    metadata[condition.field] = std::move(condition.value);
  }
  return metadata;
}

}  // namespace nvecd::server

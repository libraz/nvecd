/**
 * @file argument_validation.cpp
 * @brief Field validators shared by the TCP parser and the HTTP handlers
 */

#include "server/argument_validation.h"

#include <cctype>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

namespace nvecd::server {

namespace {

size_t ScanDigits(std::string_view text, size_t pos) {
  while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
    ++pos;
  }
  return pos;
}

size_t ScanSign(std::string_view text, size_t pos) {
  return (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ? pos + 1 : pos;
}

/// Length of the longest prefix of @p text that is a decimal literal, 0 if none.
size_t ScanDecimalLiteral(std::string_view text) {
  size_t pos = ScanSign(text, 0);
  const size_t int_end = ScanDigits(text, pos);
  bool has_digits = int_end > pos;
  pos = int_end;
  if (pos < text.size() && text[pos] == '.') {
    const size_t frac_end = ScanDigits(text, pos + 1);
    if (frac_end > pos + 1 || has_digits) {
      has_digits = has_digits || frac_end > pos + 1;
      pos = frac_end;
    }
  }
  if (!has_digits) {
    return 0;
  }
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    const size_t exp_start = ScanSign(text, pos + 1);
    const size_t exp_end = ScanDigits(text, exp_start);
    if (exp_end > exp_start) {
      pos = exp_end;
    }
  }
  return pos;
}

bool IsNonFiniteSpelling(std::string_view text) {
  const size_t start = ScanSign(text, 0);
  std::string lowered;
  for (size_t i = start; i < text.size(); ++i) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[i]))));
  }
  return lowered == "nan" || lowered == "inf" || lowered == "infinity";
}

/// Parse an integer token, saturating values beyond the int64 range.
utils::Expected<int64_t, utils::Error> ParseSaturatedInteger(std::string_view text) {
  const size_t start = (!text.empty() && text[0] == '+') ? 1 : 0;
  const char* first = text.data() + start;
  const char* last = text.data() + text.size();
  int64_t value = 0;
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ptr == first) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Failed to parse integer: " + std::string(text)));
  }
  if (ptr != last) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Invalid integer: " + std::string(text)));
  }
  if (ec == std::errc::result_out_of_range) {
    return *first == '-' ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
  }
  return value;
}

utils::Expected<int, utils::Error> CheckTopK(int64_t value, uint32_t max_top_k, std::string_view spelling) {
  if (value <= 0) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCommandInvalidTopK,
                                                  "top_k must be positive, got " + std::string(spelling)));
  }
  const int64_t limit = max_top_k > 0 ? static_cast<int64_t>(max_top_k) : std::numeric_limits<int>::max();
  if (value > limit) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandInvalidTopK,
                         "top_k " + std::string(spelling) + " exceeds maximum allowed: " + std::to_string(limit)));
  }
  return static_cast<int>(value);
}

}  // namespace

utils::Expected<uint64_t, utils::Error> ParseTimestamp(std::string_view text) {
  uint64_t value = 0;
  const char* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), last, value);
  // from_chars accepts no sign or whitespace for an unsigned type, so a
  // negative spelling is rejected here instead of wrapping.
  if (text.empty() || ptr != last || ec != std::errc()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Invalid timestamp value: " + std::string(text)));
  }
  return value;
}

utils::Expected<int, utils::Error> ValidateTopK(int64_t value, uint32_t max_top_k) {
  return CheckTopK(value, max_top_k, std::to_string(value));
}

utils::Expected<int, utils::Error> ParseTopK(std::string_view text, uint32_t max_top_k) {
  auto value = ParseSaturatedInteger(text);
  if (!value) {
    return utils::MakeUnexpected(value.error());
  }
  return CheckTopK(*value, max_top_k, text);
}

utils::Expected<int64_t, utils::Error> ParseIntegerToken(std::string_view text) {
  return ParseSaturatedInteger(text);
}

std::optional<int64_t> ParseDecimalInteger(std::string_view text) {
  const size_t start = (!text.empty() && text[0] == '+') ? 1 : 0;
  const char* first = text.data() + start;
  const char* last = text.data() + text.size();
  int64_t value = 0;
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ptr == first || ptr != last || ec != std::errc()) {
    return std::nullopt;
  }
  return value;
}

std::optional<double> ParseDecimalNumber(std::string_view text) {
  if (text.empty() || ScanDecimalLiteral(text) != text.size()) {
    return std::nullopt;
  }
  // The literal grammar is a subset of strtod's, so the whole text is consumed.
  // Underflow yields a denormal or zero and is accepted; overflow is not finite.
  const std::string literal(text);
  const double value = std::strtod(literal.c_str(), nullptr);
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

utils::Expected<float, utils::Error> NarrowToFiniteFloat(double value, std::string_view field) {
  if (!std::isfinite(value) || std::fabs(value) > static_cast<double>(FLT_MAX)) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kCommandInvalidArgument,
                                                  std::string(field) + " exceeds the finite float range"));
  }
  return static_cast<float>(value);
}

utils::Expected<float, utils::Error> ParseFloatToken(std::string_view text) {
  const size_t literal_length = ScanDecimalLiteral(text);
  if (literal_length == 0 && !IsNonFiniteSpelling(text)) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Failed to parse float: " + std::string(text)));
  }
  const auto invalid = [text]() {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kCommandInvalidArgument, "Invalid float: " + std::string(text)));
  };
  const auto value = ParseDecimalNumber(text);
  if (!value) {
    return invalid();
  }
  auto narrowed = NarrowToFiniteFloat(*value, "Float");
  if (!narrowed) {
    return invalid();
  }
  return *narrowed;
}

}  // namespace nvecd::server

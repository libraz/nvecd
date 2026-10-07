/**
 * @file argument_validation.h
 * @brief Field validators shared by the TCP parser and the HTTP handlers
 *
 * Every numeric argument both surfaces accept goes through exactly one function
 * per field kind, so TCP and HTTP accept the same value set and reject the rest
 * with a client error before any narrowing conversion. The text parsers here
 * serve the TCP tokens and metadata values; the typed validators are what both
 * the text parsers and the JSON handlers end in.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server {

/**
 * @brief Parse an EVENT timestamp: unsigned decimal digits fitting uint64
 *
 * A sign, whitespace or any other non-digit byte is rejected, so no value can
 * wrap. This is the same set a JSON unsigned integer admits on HTTP.
 */
utils::Expected<uint64_t, utils::Error> ParseTimestamp(std::string_view text);

/**
 * @brief Check a top_k value against [1, max_top_k]
 *
 * @param value Requested top_k, before any narrowing
 * @param max_top_k Configured maximum (0 = no upper bound)
 * @return The value as int, or kCommandInvalidTopK
 */
utils::Expected<int, utils::Error> ValidateTopK(int64_t value, uint32_t max_top_k);

/**
 * @brief Parse a decimal integer token as top_k
 *
 * An integer too large for int64 is saturated so it is reported as a top_k
 * range error rather than as an unparseable token.
 */
utils::Expected<int, utils::Error> ParseTopK(std::string_view text, uint32_t max_top_k);

/**
 * @brief Parse an integer token, saturating values beyond the int64 range
 *
 * "Failed to parse integer" for a token that is not a number at all, "Invalid
 * integer" for one with trailing characters. Callers echo the token, not the
 * saturated value, in their own range errors.
 */
utils::Expected<int64_t, utils::Error> ParseIntegerToken(std::string_view text);

/**
 * @brief Parse a decimal integer (optional sign, digits only)
 *
 * @return The value, or nullopt for a non-integer spelling or int64 overflow
 */
std::optional<int64_t> ParseDecimalInteger(std::string_view text);

/**
 * @brief Parse a plain decimal literal to a finite double
 *
 * Accepts an optional sign, digits with an optional fraction, and an optional
 * exponent. nan, inf and hex-float spellings are not decimal literals. A value
 * that underflows parses to its denormal or zero; one that overflows is
 * rejected.
 */
std::optional<double> ParseDecimalNumber(std::string_view text);

/**
 * @brief Narrow a double to a finite float
 *
 * Underflow narrows to a denormal or zero; a magnitude beyond the float range
 * is rejected with kCommandInvalidArgument.
 *
 * @param value Finite double to narrow
 * @param field Field name used in the error message
 */
utils::Expected<float, utils::Error> NarrowToFiniteFloat(double value, std::string_view field);

/**
 * @brief Parse a float token (vector component or min_score)
 */
utils::Expected<float, utils::Error> ParseFloatToken(std::string_view text);

}  // namespace nvecd::server

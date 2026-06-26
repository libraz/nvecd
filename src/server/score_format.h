/**
 * @file score_format.h
 * @brief Shared similarity-score formatting policy for all server surfaces
 *
 * A single formatting policy keeps SIM/SIMV scores identical across the TCP
 * dispatcher and the HTTP server. Without it, the TCP path used the default
 * ostream precision while the HTTP path used nlohmann's shortest-round-trip
 * representation, so the same score could render with a different number of
 * digits depending on the surface.
 */

#pragma once

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace nvecd::server {

/**
 * @brief Number of decimal places used when rendering similarity scores.
 *
 * Matches the documented protocol output (fixed 4 decimal places, e.g.
 * "0.9245"). Keeping it as a named constant lets both surfaces share the exact
 * same precision policy.
 */
inline constexpr int kScorePrecision = 4;

/**
 * @brief Format a similarity score with the shared fixed-precision policy.
 *
 * Renders @p score with exactly ::kScorePrecision decimal places so that
 * identical scores produce byte-identical text on every surface.
 *
 * @param score Similarity score to format
 * @return Fixed-point decimal string (e.g. "0.9245")
 */
inline std::string FormatScore(float score) {
  std::array<char, 32> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.*f", kScorePrecision, static_cast<double>(score));
  return std::string(buffer.data());
}

/**
 * @brief Round a similarity score to the shared fixed precision.
 *
 * Returns @p score rounded to ::kScorePrecision decimal places. JSON surfaces
 * use this so a numeric score serializes to the same value the TCP text surface
 * renders (e.g. 0.9245), keeping scores consistent across surfaces.
 *
 * @param score Similarity score to round
 * @return Score rounded to the shared precision
 */
inline double RoundScore(float score) {
  const double scale = std::pow(10.0, kScorePrecision);
  return std::round(static_cast<double>(score) * scale) / scale;
}

}  // namespace nvecd::server

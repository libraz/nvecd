/**
 * @file filter_parser.h
 * @brief Parse simple filter expressions into MetadataFilter
 *
 * Supports a simple key:value syntax for metadata filtering:
 *   "status:active"                   → Eq("status", "active")
 *   "status:active,category:news"     → Eq("status","active") AND Eq("category","news")
 *
 * Numeric values are auto-detected:
 *   "price:42"     → Eq("price", int64_t(42))
 *   "score:0.95"   → Eq("score", double(0.95))
 *   "active:true"  → Eq("active", bool(true))
 */

#pragma once

#include <string>

#include "utils/error.h"
#include "utils/expected.h"
#include "vectors/metadata_filter.h"

namespace nvecd::server {

/**
 * @brief Parse a simple filter expression string into a MetadataFilter
 *
 * Format: "key1:value1,key2:value2,..."
 * - Keys and values are separated by ':'
 * - Multiple conditions are separated by ',' (AND logic)
 * - Values are auto-typed: int64 > double > bool > string
 *
 * @param expr Filter expression string
 * @return MetadataFilter or error if parsing fails
 */
utils::Expected<vectors::MetadataFilter, utils::Error> ParseSimpleFilter(
    const std::string& expr);

}  // namespace nvecd::server

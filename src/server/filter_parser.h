/**
 * @file filter_parser.h
 * @brief Parse simple filter expressions into MetadataFilter
 *
 * Supports equality (`key:value` / `key=value`), comparison (`key>=value`),
 * and membership (`key=in(value1|value2)`) metadata filtering:
 *   "status:active"                   → Eq("status", "active")
 *   "status:active,category:news"     → Eq("status","active") AND Eq("category","news")
 *
 * Numeric values are auto-detected:
 *   "price:42"     → Eq("price", int64_t(42))
 *   "score:0.95"   → Eq("score", double(0.95))
 *   "active:true"  → Eq("active", bool(true))
 *   "x:nan"        → Eq("x", "nan") (only finite decimal literals become numbers)
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
 * - Values are auto-typed: bool > int64 > double > string
 *
 * @param expr Filter expression string
 * @return MetadataFilter or error if parsing fails
 */
utils::Expected<vectors::MetadataFilter, utils::Error> ParseSimpleFilter(const std::string& expr);

/**
 * @brief Parse a METASET pair list into metadata
 *
 * Uses the filter grammar with values typed the same way, but accepts only
 * equality pairs (`key:value` / `key=value`): a comparison, an in(...) list or
 * an empty list is rejected so nothing but the written values is ever stored.
 *
 * @param expr Pair list, e.g. "category:books,price:12"
 * @return Metadata or kCommandInvalidArgument / kCommandParseError
 */
utils::Expected<vectors::Metadata, utils::Error> ParseMetadataPairs(const std::string& expr);

}  // namespace nvecd::server

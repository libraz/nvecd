/**
 * @file command_types.h
 * @brief Command type definitions for nvecd protocol
 *
 * Reference: ../mygram-db/src/query/query_types.h
 * Reusability: 30% (completely different command set for nvecd)
 */

#pragma once

#include <string>

namespace nvecd::server {

/**
 * @brief Command types supported by nvecd
 *
 * Core commands:
 * - EVENT: Ingest co-occurrence event
 * - VECSET: Register vector embedding
 * - METASET: Register item metadata
 * - SIM: Similarity search by ID
 * - SIMV: Similarity search by vector
 *
 * Admin commands (MygramDB-compatible):
 * - INFO: Server statistics
 * - CONFIG: Configuration management
 * - DUMP: Snapshot management
 * - DEBUG: Debug mode toggle
 */
enum class CommandType : std::uint8_t {
  // Core commands
  kEvent,
  kVecset,
  kVecdel,
  kMetaset,
  kSim,
  kSimv,

  // Admin commands
  kInfo,
  kConfigHelp,
  kConfigShow,
  kConfigVerify,
  kDumpSave,
  kDumpLoad,
  kDumpVerify,
  kDumpInfo,
  kDumpStatus,
  kDebugOn,
  kDebugOff,
  kCacheStats,
  kCacheClear,
  kCacheEnable,
  kCacheDisable,

  // Variable commands (SET/SHOW VARIABLES)
  kSet,
  kGet,
  kShowVariables,

  // Auth command
  kAuth,

  // Special
  kUnknown,

  /// Sentinel holding the number of declared command types. Every enumerator
  /// must be added before it, so a compile-time table sized by kCount fails to
  /// build until the new command is classified.
  kCount
};

/**
 * @brief Convert CommandType to string
 */
inline const char* CommandTypeToString(CommandType type) {
  switch (type) {
    case CommandType::kEvent:
      return "EVENT";
    case CommandType::kVecset:
      return "VECSET";
    case CommandType::kVecdel:
      return "VECDEL";
    case CommandType::kMetaset:
      return "METASET";
    case CommandType::kSim:
      return "SIM";
    case CommandType::kSimv:
      return "SIMV";
    case CommandType::kInfo:
      return "INFO";
    case CommandType::kConfigHelp:
      return "CONFIG_HELP";
    case CommandType::kConfigShow:
      return "CONFIG_SHOW";
    case CommandType::kConfigVerify:
      return "CONFIG_VERIFY";
    case CommandType::kDumpSave:
      return "DUMP_SAVE";
    case CommandType::kDumpLoad:
      return "DUMP_LOAD";
    case CommandType::kDumpVerify:
      return "DUMP_VERIFY";
    case CommandType::kDumpInfo:
      return "DUMP_INFO";
    case CommandType::kDumpStatus:
      return "DUMP_STATUS";
    case CommandType::kDebugOn:
      return "DEBUG_ON";
    case CommandType::kDebugOff:
      return "DEBUG_OFF";
    case CommandType::kCacheStats:
      return "CACHE_STATS";
    case CommandType::kCacheClear:
      return "CACHE_CLEAR";
    case CommandType::kCacheEnable:
      return "CACHE_ENABLE";
    case CommandType::kCacheDisable:
      return "CACHE_DISABLE";
    case CommandType::kSet:
      return "SET";
    case CommandType::kGet:
      return "GET";
    case CommandType::kShowVariables:
      return "SHOW_VARIABLES";
    case CommandType::kAuth:
      return "AUTH";
    case CommandType::kUnknown:
    case CommandType::kCount:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

/**
 * @brief Command privilege level for authorization
 */
enum class CommandPrivilege : std::uint8_t {
  kRead,   ///< Read-only commands (always allowed)
  kWrite,  ///< Write commands (require auth when password set)
  kAdmin   ///< Admin commands (require auth when password set)
};

/**
 * @brief Get privilege level for a command type
 *
 * This is the single authority for "may an unauthenticated client run this?",
 * consumed by both the TCP dispatcher and the HTTP route table. Every
 * enumerator is listed explicitly and there is no catch-all: a command type
 * added without a classification is reported by -Wswitch, and the unreachable
 * fallback after the switch is kAdmin so an unclassified value fails closed
 * rather than becoming an open read.
 */
inline CommandPrivilege GetCommandPrivilege(CommandType type) {
  switch (type) {
    case CommandType::kEvent:
    case CommandType::kVecset:
    case CommandType::kVecdel:
    case CommandType::kMetaset:
    case CommandType::kSet:
    case CommandType::kCacheClear:
    case CommandType::kCacheEnable:
    case CommandType::kCacheDisable:
      return CommandPrivilege::kWrite;

    case CommandType::kDumpSave:
    case CommandType::kDumpLoad:
    case CommandType::kDumpVerify:
    case CommandType::kDumpInfo:
    case CommandType::kDumpStatus:
    case CommandType::kConfigVerify:
      return CommandPrivilege::kAdmin;

    case CommandType::kSim:
    case CommandType::kSimv:
    case CommandType::kInfo:
    case CommandType::kConfigHelp:
    case CommandType::kConfigShow:
    case CommandType::kDebugOn:
    case CommandType::kDebugOff:
    case CommandType::kCacheStats:
    case CommandType::kGet:
    case CommandType::kShowVariables:
      return CommandPrivilege::kRead;

    // AUTH is answered before the gate, an unparsed command never reaches a
    // handler, and kCount is not a command. None of them is an open read.
    case CommandType::kAuth:
    case CommandType::kUnknown:
    case CommandType::kCount:
      return CommandPrivilege::kAdmin;
  }
  return CommandPrivilege::kAdmin;
}

}  // namespace nvecd::server

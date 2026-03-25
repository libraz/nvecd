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
  kUnknown
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
 */
inline CommandPrivilege GetCommandPrivilege(CommandType type) {
  switch (type) {
    case CommandType::kEvent:
    case CommandType::kVecset:
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
    default:
      return CommandPrivilege::kRead;
  }
}

}  // namespace nvecd::server

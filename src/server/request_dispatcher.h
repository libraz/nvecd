/**
 * @file request_dispatcher.h
 * @brief Request dispatcher for routing commands to handlers
 *
 * Reference: ../mygram-db/src/server/request_dispatcher.h
 * Reusability: 85% (similar pattern, different command types)
 */

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "server/command_parser.h"
#include "server/server_types.h"
#include "storage/wal.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server {

/**
 * @brief Result of a successfully applied write command
 */
struct WriteOutcome {
  CommandType type = CommandType::kUnknown;

  /// EVENT only: the event was a duplicate of one already in the dedup window,
  /// so no WAL record was written and no co-occurrence edge was ingested.
  bool deduplicated = false;

  /// VECSET only: dimension of the stored vector.
  size_t dimension = 0;
};

/**
 * @brief Apply a write command to the live stores
 *
 * The single entry point for every mutation, whatever surface it arrived on.
 * Input validation, the WAL append, the store mutation, the ANN notification,
 * the cache-generation bump and cache invalidation happen here and nowhere
 * else, so the side-effect set of a write cannot depend on whether it came in
 * over TCP or HTTP. Callers do parsing and serialisation only.
 *
 * The command-type switch has no default label: adding a write command that is
 * not classified here is reported by the compiler instead of silently
 * returning "unknown command" at run time.
 *
 * WAL ordering: the mutation and its WAL append are serialised against other
 * writers through HandlerContext::write_serialization_gate, which is acquired
 * here. Snapshot admission (the loading/read-only flags and
 * HandlerContext::snapshot_write_gate) stays with the caller, because a
 * rejected write must be reported in that surface's own wire form.
 *
 * @param ctx Handler context owning the stores, WAL and cache
 * @param cmd Parsed command; must be a write command
 * @return Outcome of the write, or the error the client must be told about
 */
utils::Expected<WriteOutcome, utils::Error> ApplyWrite(HandlerContext& ctx, const Command& cmd);

/**
 * @brief Whether the read-only state refuses a command, on either surface
 *
 * The one admission decision for both TCP and HTTP. Only store-mutating
 * commands (EVENT, VECSET, VECDEL, METASET) are refused; reads and admin
 * commands reach their handlers, which report their own concurrency errors.
 * The refusal names the actual cause: a lock-mode snapshot holding the write
 * barrier, or the durability fail-stop latch.
 *
 * @param ctx Handler context holding both flags
 * @param type Command being admitted
 * @param message Receives the refusal text when the command is refused
 * @return true when the command must be refused
 */
bool IsBlockedByReadOnly(const HandlerContext& ctx, CommandType type, std::string* message);

/**
 * @brief Result of a SIM or SIMV search, before surface formatting
 */
struct SearchOutcome {
  /// Results that passed min_score, in rank order.
  std::vector<std::pair<std::string, float>> results;

  /// Results before min_score was applied (the candidate count DEBUG reports).
  size_t candidate_count = 0;

  /// Engine search plus post-filter time; 0 when served from the cache.
  double elapsed_ms = 0.0;
};

/**
 * @brief Run a SIM or SIMV command through the cache and the engine
 *
 * The single search path for both surfaces, so the cache key, the cache
 * admission check and the stored query cost cannot differ by surface. The
 * caller has already parsed the filter into Command::filter and keeps the
 * snapshot admission, exactly as for ApplyWrite.
 *
 * @param ctx Handler context owning the engine and cache
 * @param cmd Parsed SIM or SIMV command
 * @return The search outcome, or the error the client must be told about
 */
utils::Expected<SearchOutcome, utils::Error> ExecuteSearch(HandlerContext& ctx, const Command& cmd);

/**
 * @brief Request dispatcher
 *
 * This class parses commands and routes them to appropriate handlers.
 * It contains pure application logic with no network dependencies.
 *
 * Key responsibilities:
 * - Parse incoming request strings using CommandParser
 * - Validate commands (check server state, etc.)
 * - Route to appropriate handler logic
 * - Format responses
 * - Handle errors gracefully
 *
 * Design principles:
 * - Pure logic, no threading or I/O
 * - Easy to unit test
 * - Clear separation from network layer
 * - Uses Expected<T, Error> for type-safe error handling
 */
class RequestDispatcher {
 public:
  /**
   * @brief Construct a RequestDispatcher
   * @param handler_ctx Handler context (contains core components)
   */
  explicit RequestDispatcher(HandlerContext& handler_ctx);

  // Disable copy and move
  RequestDispatcher(const RequestDispatcher&) = delete;
  RequestDispatcher& operator=(const RequestDispatcher&) = delete;
  RequestDispatcher(RequestDispatcher&&) = delete;
  RequestDispatcher& operator=(RequestDispatcher&&) = delete;

  ~RequestDispatcher() = default;

  /**
   * @brief Dispatch a request to appropriate handler
   * @param request Single-line request string
   * @param conn_ctx Connection context
   * @return Response string
   */
  std::string Dispatch(const std::string& request, ConnectionContext& conn_ctx);

  /**
   * @brief Re-apply a single WAL record during crash recovery
   *
   * Decodes @p record into a Command and applies it through ApplyWrite so the
   * in-memory state is reconstructed by the same code path live traffic uses.
   * This must never re-append to the WAL: callers perform replay while
   * HandlerContext::wal is still null, so the append inside ApplyWrite is
   * skipped.
   *
   * Recovery is fail-closed. A decode failure, a CRC mismatch, a torn record
   * or any apply error other than the cases below is corruption: it is
   * returned as an error and aborts replay rather than being skipped, because
   * continuing past it would substitute silent state divergence for a visible
   * startup failure.
   *
   * The exceptions are the records IsIntendedReplayGap() classifies: one whose
   * subject this configuration cannot restore (a kVectorNotFound on a replayed
   * VECDEL or METASET, which a log written under `wal.include_vectors: false`
   * necessarily produces), and one that validation added after it was logged
   * now rejects. Skipping only those keeps such a server startable. Every skip
   * is logged with a running total, which INFO also reports.
   *
   * @param record WAL record produced by WriteAheadLog::Replay
   * @return Empty on success or on an intended gap; an error on corruption
   */
  utils::Expected<void, utils::Error> ReplayRecord(const storage::WalRecord& record);

 private:
  /**
   * @brief Run a write command through ApplyWrite and render its TCP response
   *
   * Keeps the dispatcher free of write semantics: everything that mutates
   * state lives behind ApplyWrite.
   */
  utils::Expected<std::string, utils::Error> HandleWrite(const Command& cmd) const;

  /**
   * @brief Run SIM or SIMV through ExecuteSearch and render its TCP response
   */
  utils::Expected<std::string, utils::Error> HandleSearch(const Command& cmd, ConnectionContext& conn_ctx) const;

  // Handler methods
  utils::Expected<std::string, utils::Error> HandleInfo(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleConfigHelp(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleConfigShow(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleConfigVerify(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleDumpSave(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleDumpLoad(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleDumpVerify(const Command& cmd) const;
  utils::Expected<std::string, utils::Error> HandleDumpInfo(const Command& cmd) const;
  utils::Expected<std::string, utils::Error> HandleDumpStatus();
  static utils::Expected<std::string, utils::Error> HandleDebugOn(ConnectionContext& conn_ctx);
  static utils::Expected<std::string, utils::Error> HandleDebugOff(ConnectionContext& conn_ctx);

  // Auth handler
  utils::Expected<std::string, utils::Error> HandleAuth(const Command& cmd, ConnectionContext& conn_ctx) const;

  // Variable command handlers
  utils::Expected<std::string, utils::Error> HandleSet(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleGet(const Command& cmd);
  utils::Expected<std::string, utils::Error> HandleShowVariables(const Command& cmd);

  // Format response helpers
  static std::string FormatOK(const std::string& msg = "");
  static std::string FormatError(const std::string& msg);
  static std::string FormatSimResults(const std::vector<std::pair<std::string, float>>& results, int count);

  HandlerContext& ctx_;
};

}  // namespace nvecd::server

/**
 * @file dump_handler.h
 * @brief DUMP command handlers (SAVE, LOAD, VERIFY, INFO)
 *
 * Reference: ../mygram-db/src/server/handlers/dump_handler.h
 * Reusability: 90%
 */

#pragma once

#include <string>

#include "server/server_types.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server::handlers {

/**
 * @brief Start a background fork snapshot of the live stores
 *
 * The one routine every fork capture goes through: DUMP SAVE in fork mode, the
 * auto-snapshot scheduler and WAL-off decay maintenance. It holds the snapshot
 * gate shared and the write serialization gate across the WAL sequence capture
 * and fork(), so no write sits between its WAL append and its store apply, and
 * it refuses while a DUMP LOAD is in flight. The captured sequence therefore
 * equals exactly what the forked image contains.
 *
 * @param ctx Handler context (stores, config, fork writer, gates)
 * @param resolved_path Validated snapshot path
 * @return Success once the child exists, or an error
 */
utils::Expected<void, utils::Error> StartForkSnapshot(HandlerContext& ctx, const std::string& resolved_path);

/**
 * @brief Handle DUMP SAVE
 *
 * With no @p filepath the snapshot goes to `snapshot.default_filename` inside
 * the dump directory, and only to a timestamped name when that setting is
 * empty. Both forms are resolved through the same dump-path validation.
 *
 * @param ctx Handler context (stores, config, snapshot writer, WAL)
 * @param filepath Client-supplied path, or empty to use the configured default
 * @return Wire response, or an error
 */
utils::Expected<std::string, utils::Error> HandleDumpSave(HandlerContext& ctx, const std::string& filepath);

/**
 * @brief Handle DUMP LOAD command
 *
 * Loads a snapshot from the specified filepath into the stores. Sets loading
 * mode during the load operation. Filepath is required.
 *
 * @param ctx Handler context (must have event_store, co_index, vector_store)
 * @param filepath Source file path (must not be empty)
 * @return OK response with loaded path, or error
 */
utils::Expected<std::string, utils::Error> HandleDumpLoad(HandlerContext& ctx, const std::string& filepath);

/**
 * @brief Handle DUMP VERIFY command
 *
 * Verifies the integrity of a snapshot file. Filepath is required.
 *
 * @param dump_dir The dump directory for path validation
 * @param filepath Snapshot file path to verify (must not be empty)
 * @return OK response on success, or error with integrity details
 */
utils::Expected<std::string, utils::Error> HandleDumpVerify(const std::string& dump_dir, const std::string& filepath);

/**
 * @brief Handle DUMP INFO command
 *
 * Reads and returns metadata from a snapshot file. Filepath is required.
 *
 * @param dump_dir The dump directory for path validation
 * @param filepath Snapshot file path to inspect (must not be empty)
 * @return Formatted snapshot info response, or error
 */
utils::Expected<std::string, utils::Error> HandleDumpInfo(const std::string& dump_dir, const std::string& filepath);

/**
 * @brief Handle DUMP STATUS command
 *
 * Returns the status of the background fork snapshot operation.
 *
 * @param ctx Handler context (must have fork_snapshot_writer)
 * @return Formatted status response, or error
 */
utils::Expected<std::string, utils::Error> HandleDumpStatus(HandlerContext& ctx);

}  // namespace nvecd::server::handlers

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
 * @brief Handle DUMP SAVE command
 *
 * Saves a snapshot of all stores to the specified filepath. If filepath is
 * empty, generates a timestamped default filename. Sets read-only mode
 * during the save operation.
 *
 * @param ctx Handler context (must have config, event_store, co_index, vector_store)
 * @param filepath Target file path (empty for auto-generated name)
 * @return OK response with saved path, or error
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

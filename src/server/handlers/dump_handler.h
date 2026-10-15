/**
 * @file dump_handler.h
 * @brief DUMP command handlers (SAVE, LOAD, VERIFY, INFO)
 *
 * Reference: ../mygram-db/src/server/handlers/dump_handler.h
 * Reusability: 90%
 */

#pragma once

#include <memory>
#include <string>

#include "config/config.h"
#include "events/co_occurrence_index.h"
#include "events/event_store.h"
#include "server/server_types.h"
#include "utils/error.h"
#include "utils/expected.h"
#include "vectors/metadata_store.h"
#include "vectors/vector_store.h"

namespace nvecd::server::handlers {

/**
 * @brief Co-occurrence index settings derived from the events configuration
 */
events::CoOccurrenceIndex::Config MakeCoOccurrenceConfig(const config::EventsConfig& events);

/**
 * @brief Stores a snapshot is deserialized into before it replaces live state
 *
 * A corrupt or semantically invalid file can only touch these instances. Startup
 * recovery and DUMP LOAD both stage, read, then Publish().
 */
class StagedStores {
 public:
  /**
   * @param config Server configuration the stores are sized from
   * @param with_metadata Whether to stage a metadata store
   */
  StagedStores(const config::Config& config, bool with_metadata);

  /// Deserialize the snapshot at @p path into the staged stores.
  utils::Expected<void, utils::Error> Read(const std::string& path);

  /// @p error's message, followed by the integrity detail the last Read() reported.
  std::string Describe(const utils::Error& error) const;

  /**
   * @brief Swap the staged state into the live stores of @p ctx
   *
   * Optionally rebuilds the ANN index, then bumps the vector, metadata and dataset
   * generations and clears the similarity cache, so no search cached against the
   * old index outlives the swap. Startup passes false and rebuilds after WAL replay.
   * Callers hold whatever gates keep writers out.
   */
  void Publish(HandlerContext& ctx, bool rebuild_ann);

 private:
  events::EventStore event_store_;
  events::CoOccurrenceIndex co_index_;
  vectors::VectorStore vector_store_;
  std::unique_ptr<vectors::MetadataStore> metadata_store_;
  std::string integrity_message_;
};

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

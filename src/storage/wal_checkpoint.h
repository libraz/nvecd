/**
 * @file wal_checkpoint.h
 * @brief Snapshot/WAL checkpoint sidecar and recovery-base selection
 *
 * A snapshot records the WAL sequence number it already contains so that, on
 * recovery, only WAL records newer than that sequence need to be replayed. To
 * avoid changing the binary snapshot format, the sequence is stored in a small
 * sidecar file alongside the snapshot, named "<snapshot_path>.walseq".
 *
 * Sidecar format: a fixed 32-byte, versioned and CRC-protected frame containing
 * the WAL sequence plus the bound snapshot's size and whole-file CRC32.
 *
 * This module also decides which snapshot is a recovery base: the candidate
 * order, the sidecar requirement, and the order in which a file is bound and
 * then made newest. Startup recovery, retention and DUMP LOAD all go through it.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::storage {

/// Filename suffix appended to the snapshot path for the WAL checkpoint sidecar.
constexpr const char* kWalCheckpointSuffix = ".walseq";

/// Path of the checkpoint sidecar belonging to a snapshot path.
std::string WalCheckpointPath(const std::string& snapshot_path);

/**
 * @brief Write the WAL sequence checkpoint sidecar for a snapshot
 *
 * Writes a versioned checkpoint frame to "<snapshot_path>.walseq". The
 * write is atomic: the data is first written to a ".tmp" file which is then
 * renamed over the final sidecar path.
 *
 * @param snapshot_path Path of the snapshot the checkpoint belongs to
 * @param sequence WAL sequence number contained in the snapshot
 * @return Success, or a kStorage* error on I/O failure
 */
utils::Expected<void, utils::Error> WriteWalCheckpoint(const std::string& snapshot_path, uint64_t sequence);

/**
 * @brief Read the WAL sequence checkpoint sidecar for a snapshot
 *
 * @param snapshot_path Path of the snapshot whose checkpoint should be read
 * @return The validated stored sequence number, or an error for any absent,
 *         malformed, unsafe, unbound, or unreadable sidecar
 */
utils::Expected<uint64_t, utils::Error> ReadWalCheckpoint(const std::string& snapshot_path);

/**
 * @brief Snapshots in @p dir recovery may select, newest first
 *
 * Only a final recoverable extension qualifies, so writer temporaries and
 * sidecars never do. With @p require_checkpoint a file also needs a sidecar to
 * exist; whether it validates is RecoveryCheckpoint()'s decision. Ordered by
 * modification time, then path, descending.
 *
 * @param dir Snapshot directory to scan (a missing directory yields no candidates)
 * @param require_checkpoint Whether the WAL is enabled
 */
std::vector<std::string> ListRecoveryCandidates(const std::string& dir, bool require_checkpoint);

/**
 * @brief Whether @p snapshot_path is a recovery base, and the WAL floor it carries
 *
 * @param snapshot_path Snapshot to judge
 * @param require_checkpoint Whether the WAL is enabled
 * @return 0 when no checkpoint is required; otherwise the sequence of a sidecar
 *         that validates against the snapshot, or the reason it does not
 */
utils::Expected<uint64_t, utils::Error> RecoveryCheckpoint(const std::string& snapshot_path, bool require_checkpoint);

/**
 * @brief Bind @p snapshot_path to @p sequence and make it the newest recovery base
 *
 * The sidecar is written first and the modification time advanced only after,
 * so a failure at either step leaves the previously newest base selected.
 *
 * @param snapshot_path Snapshot that now contains every record up to @p sequence
 * @param sequence WAL sequence the snapshot contains
 * @return Success, or the failed step's error
 */
utils::Expected<void, utils::Error> BindRecoveryBase(const std::string& snapshot_path, uint64_t sequence);

}  // namespace nvecd::storage

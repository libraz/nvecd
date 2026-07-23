/**
 * @file wal_checkpoint.h
 * @brief Snapshot/WAL checkpoint sidecar helpers
 *
 * A snapshot records the WAL sequence number it already contains so that, on
 * recovery, only WAL records newer than that sequence need to be replayed. To
 * avoid changing the binary snapshot format, the sequence is stored in a small
 * sidecar file alongside the snapshot, named "<snapshot_path>.walseq".
 *
 * Sidecar format: a fixed 32-byte, versioned and CRC-protected frame containing
 * the WAL sequence plus the bound snapshot's size and whole-file CRC32.
 */

#pragma once

#include <cstdint>
#include <string>

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::storage {

/// Filename suffix appended to the snapshot path for the WAL checkpoint sidecar.
constexpr const char* kWalCheckpointSuffix = ".walseq";

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

}  // namespace nvecd::storage

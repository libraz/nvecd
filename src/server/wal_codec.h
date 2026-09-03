/**
 * @file wal_codec.h
 * @brief Binary codec mapping write Commands to/from WAL record payloads
 *
 * Provides a stable little-endian binary encoding for the write commands that
 * are persisted to the Write-Ahead Log (EVENT, VECSET, VECDEL, METASET). The codec is
 * symmetric: EncodeCommand() produces the payload bytes stored in a WAL record,
 * and DecodeWalRecord() reconstructs the original Command from a replayed
 * record.
 *
 * All integers are encoded little-endian. Every string is length-prefixed with
 * a uint32 byte length followed by the raw bytes (no terminator).
 *
 * Payload layouts (op type comes from the WAL record header, not the payload):
 *   EVENT   : u8 event_type, str ctx, str id, i32 score, u64 timestamp
 *   VECSET  : str id, u32 dim, dim * f32 (IEEE-754 little-endian)
 *   VECDEL  : str id
 *   METASET : str id, either legacy str filter_expr or a versioned typed
 *             metadata map (used by HTTP so JSON values round-trip exactly)
 *
 * For EVENT, the event type is embedded in the payload (in addition to driving
 * the WAL op type) so that decode reconstructs the exact EventType. The METASET
 * payload stores cmd.filter_expr verbatim so the wiring pass can re-run the
 * metadata handler and re-parse it identically. Typed HTTP records instead
 * populate Command::metadata.
 */

#pragma once

#include <cstdint>
#include <vector>

#include "server/command_parser.h"
#include "storage/wal.h"
#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server {

/**
 * @brief Map a write Command to its WAL operation type
 *
 * EVENT maps to kEventDel when the command's event_type is the delete type,
 * otherwise kEventAdd. VECSET/VECDEL map to kVecSet/kVecDel, METASET maps to kMetaSet.
 *
 * @param cmd Command to classify (must be EVENT, VECSET, VECDEL, or METASET)
 * @return Corresponding WAL operation type
 */
storage::WalOpType WalOpForCommand(const Command& cmd);

/**
 * @brief Encode a write Command into WAL payload bytes
 *
 * Only EVENT, VECSET, VECDEL, and METASET commands are encodable. For any other command
 * type an empty buffer is returned.
 *
 * @param cmd Command to encode
 * @return Little-endian payload bytes, or an empty vector for unsupported types
 */
std::vector<uint8_t> EncodeCommand(const Command& cmd);

/**
 * @brief Reconstruct a Command from a replayed WAL record
 *
 * Reads the WAL record op type and payload and rebuilds the original Command,
 * including the command type and type-specific fields. EVENT records populate
 * event_type and the optional timestamp; VECSET records populate the vector and
 * dimension; METASET records populate filter_expr or typed metadata.
 *
 * @param record WAL record produced by Replay()
 * @return Decoded Command, or a kStorage* error on a truncated/invalid payload
 */
utils::Expected<Command, utils::Error> DecodeWalRecord(const storage::WalRecord& record);

/**
 * @brief Whether a replay failure is an intended recovery gap, not corruption
 *
 * With `wal.include_vectors: false` the server deliberately omits VECSET
 * payloads from the log, so a later VECDEL or METASET record can legitimately
 * refer to a vector that neither the snapshot nor the WAL can restore. Replay
 * must skip those records and continue; stopping on them makes a server that
 * was configured this way unable to start again, with no recovery other than
 * deleting the WAL by hand.
 *
 * The tolerated set is deliberately narrow. Only a missing vector, and only for
 * the two operations whose subject the configuration is allowed to omit, is a
 * gap. Every other operation, and every other error — CRC mismatch, truncation,
 * decode failure, an invalid payload — stays fail-closed, because those mean the
 * log does not say what the server wrote.
 *
 * @param op WAL operation type of the record being replayed
 * @param code Error code returned by the handler that applied the record
 * @return true when the record may be skipped and replay continued
 */
bool IsIntendedReplayGap(storage::WalOpType op, utils::ErrorCode code);

}  // namespace nvecd::server

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

}  // namespace nvecd::server

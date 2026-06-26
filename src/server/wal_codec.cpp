/**
 * @file wal_codec.cpp
 * @brief Binary codec implementation for WAL command payloads
 */

#include "server/wal_codec.h"

#include <cstring>
#include <string>
#include <type_traits>

namespace nvecd::server {

namespace {

using nvecd::events::EventType;
using storage::WalOpType;
using storage::WalRecord;

/// Append a little-endian integer of type T to the buffer.
template <typename T>
void AppendLE(std::vector<uint8_t>& buf, T value) {
  static_assert(std::is_integral<T>::value, "AppendLE requires an integral type");
  for (size_t i = 0; i < sizeof(T); ++i) {
    buf.push_back(static_cast<uint8_t>((static_cast<uint64_t>(value) >> (8 * i)) & 0xFF));
  }
}

/// Append a length-prefixed string (u32 length + raw bytes).
void AppendString(std::vector<uint8_t>& buf, const std::string& str) {
  AppendLE<uint32_t>(buf, static_cast<uint32_t>(str.size()));
  buf.insert(buf.end(), str.begin(), str.end());
}

/// Append a float as IEEE-754 little-endian bytes.
void AppendFloat(std::vector<uint8_t>& buf, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendLE<uint32_t>(buf, bits);
}

/// Cursor for safely reading from a payload buffer with bounds checking.
class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  /// Read a little-endian integer of type T.
  bool ReadLE(uint64_t& out, size_t bytes) {
    if (pos_ + bytes > size_) {
      return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) {
      value |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
    }
    pos_ += bytes;
    out = value;
    return true;
  }

  /// Read a length-prefixed string into out.
  bool ReadString(std::string& out) {
    uint64_t len = 0;
    if (!ReadLE(len, sizeof(uint32_t))) {
      return false;
    }
    if (pos_ + len > size_) {
      return false;
    }
    out.assign(reinterpret_cast<const char*>(data_ + pos_),
               len);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    pos_ += len;
    return true;
  }

  /// Read a single IEEE-754 little-endian float into out.
  bool ReadFloat(float& out) {
    uint64_t bits = 0;
    if (!ReadLE(bits, sizeof(uint32_t))) {
      return false;
    }
    uint32_t bits32 = static_cast<uint32_t>(bits);
    std::memcpy(&out, &bits32, sizeof(out));
    return true;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

utils::Error TruncatedError(const char* what) {
  return utils::MakeError(utils::ErrorCode::kStorageCorrupted,
                          std::string("WAL payload truncated while decoding ") + what);
}

}  // namespace

storage::WalOpType WalOpForCommand(const Command& cmd) {
  switch (cmd.type) {
    case CommandType::kEvent:
      return cmd.event_type == EventType::DEL ? WalOpType::kEventDel : WalOpType::kEventAdd;
    case CommandType::kVecset:
      return WalOpType::kVecSet;
    case CommandType::kMetaset:
      return WalOpType::kMetaSet;
    default:
      // Unsupported command types should never reach the WAL.
      return WalOpType::kVecSet;
  }
}

std::vector<uint8_t> EncodeCommand(const Command& cmd) {
  std::vector<uint8_t> buf;

  switch (cmd.type) {
    case CommandType::kEvent: {
      // EVENT: u8 event_type, str ctx, str id, i32 score, u64 timestamp
      buf.push_back(static_cast<uint8_t>(cmd.event_type));
      AppendString(buf, cmd.ctx);
      AppendString(buf, cmd.id);
      AppendLE<uint32_t>(buf, static_cast<uint32_t>(static_cast<int32_t>(cmd.score)));
      AppendLE<uint64_t>(buf, cmd.timestamp.value_or(0));
      break;
    }
    case CommandType::kVecset: {
      // VECSET: str id, u32 dim, dim * f32
      AppendString(buf, cmd.id);
      AppendLE<uint32_t>(buf, static_cast<uint32_t>(cmd.vector.size()));
      for (float value : cmd.vector) {
        AppendFloat(buf, value);
      }
      break;
    }
    case CommandType::kMetaset: {
      // METASET: str id, str filter_expr
      AppendString(buf, cmd.id);
      AppendString(buf, cmd.filter_expr);
      break;
    }
    default:
      // Unsupported command type: return an empty buffer.
      buf.clear();
      break;
  }

  return buf;
}

utils::Expected<Command, utils::Error> DecodeWalRecord(const storage::WalRecord& record) {
  Reader reader(record.payload.data(), record.payload.size());
  Command cmd;

  switch (record.op) {
    case WalOpType::kEventAdd:
    case WalOpType::kEventDel: {
      cmd.type = CommandType::kEvent;
      uint64_t event_type_raw = 0;
      if (!reader.ReadLE(event_type_raw, sizeof(uint8_t))) {
        return utils::MakeUnexpected(TruncatedError("EVENT event_type"));
      }
      cmd.event_type = static_cast<EventType>(static_cast<uint8_t>(event_type_raw));
      if (!reader.ReadString(cmd.ctx)) {
        return utils::MakeUnexpected(TruncatedError("EVENT ctx"));
      }
      if (!reader.ReadString(cmd.id)) {
        return utils::MakeUnexpected(TruncatedError("EVENT id"));
      }
      uint64_t score_raw = 0;
      if (!reader.ReadLE(score_raw, sizeof(uint32_t))) {
        return utils::MakeUnexpected(TruncatedError("EVENT score"));
      }
      cmd.score = static_cast<int>(static_cast<int32_t>(static_cast<uint32_t>(score_raw)));
      uint64_t timestamp = 0;
      if (!reader.ReadLE(timestamp, sizeof(uint64_t))) {
        return utils::MakeUnexpected(TruncatedError("EVENT timestamp"));
      }
      cmd.timestamp = timestamp;
      break;
    }
    case WalOpType::kVecSet: {
      cmd.type = CommandType::kVecset;
      if (!reader.ReadString(cmd.id)) {
        return utils::MakeUnexpected(TruncatedError("VECSET id"));
      }
      uint64_t dim = 0;
      if (!reader.ReadLE(dim, sizeof(uint32_t))) {
        return utils::MakeUnexpected(TruncatedError("VECSET dim"));
      }
      cmd.dimension = static_cast<int>(dim);
      cmd.vector.reserve(dim);
      for (uint64_t i = 0; i < dim; ++i) {
        float value = 0.0F;
        if (!reader.ReadFloat(value)) {
          return utils::MakeUnexpected(TruncatedError("VECSET vector element"));
        }
        cmd.vector.push_back(value);
      }
      break;
    }
    case WalOpType::kMetaSet: {
      cmd.type = CommandType::kMetaset;
      if (!reader.ReadString(cmd.id)) {
        return utils::MakeUnexpected(TruncatedError("METASET id"));
      }
      if (!reader.ReadString(cmd.filter_expr)) {
        return utils::MakeUnexpected(TruncatedError("METASET filter_expr"));
      }
      break;
    }
    default:
      return utils::MakeUnexpected(
          utils::MakeError(utils::ErrorCode::kStorageInvalidFormat, "Unsupported WAL op type for command decode"));
  }

  return cmd;
}

}  // namespace nvecd::server

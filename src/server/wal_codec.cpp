/**
 * @file wal_codec.cpp
 * @brief Binary codec implementation for WAL command payloads
 */

#include "server/wal_codec.h"

#include <cstring>
#include <limits>
#include <string>
#include <type_traits>

namespace nvecd::server {

namespace {

using nvecd::events::EventType;
using storage::WalOpType;
using storage::WalRecord;

constexpr uint32_t kTypedMetadataMagic = 0x314D564EU;  // Bytes: "NVM1"

enum class MetadataValueTag : uint8_t { kString, kInt64, kDouble, kBool };

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

void AppendDouble(std::vector<uint8_t>& buf, double value) {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendLE<uint64_t>(buf, bits);
}

/// Cursor for safely reading from a payload buffer with bounds checking.
class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  /// Read a little-endian integer of type T.
  bool ReadLE(uint64_t& out, size_t bytes) {
    if (bytes > remaining()) {
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
    if (len > remaining()) {
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

  bool ReadDouble(double& out) {
    uint64_t bits = 0;
    if (!ReadLE(bits, sizeof(uint64_t))) {
      return false;
    }
    std::memcpy(&out, &bits, sizeof(out));
    return true;
  }

  size_t position() const { return pos_; }
  void set_position(size_t position) { pos_ = position; }
  size_t remaining() const { return size_ - pos_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

utils::Error TruncatedError(const char* what) {
  return utils::MakeError(utils::ErrorCode::kStorageCorrupted,
                          std::string("WAL payload truncated while decoding ") + what);
}

void EncodeTypedMetadata(std::vector<uint8_t>& buf, const vectors::Metadata& metadata) {
  AppendLE<uint32_t>(buf, kTypedMetadataMagic);
  AppendLE<uint32_t>(buf, static_cast<uint32_t>(metadata.size()));
  for (const auto& [key, value] : metadata) {
    AppendString(buf, key);
    std::visit(
        [&buf](const auto& held) {
          using T = std::decay_t<decltype(held)>;
          if constexpr (std::is_same_v<T, std::string>) {
            buf.push_back(static_cast<uint8_t>(MetadataValueTag::kString));
            AppendString(buf, held);
          } else if constexpr (std::is_same_v<T, int64_t>) {
            buf.push_back(static_cast<uint8_t>(MetadataValueTag::kInt64));
            AppendLE<int64_t>(buf, held);
          } else if constexpr (std::is_same_v<T, double>) {
            buf.push_back(static_cast<uint8_t>(MetadataValueTag::kDouble));
            AppendDouble(buf, held);
          } else {
            buf.push_back(static_cast<uint8_t>(MetadataValueTag::kBool));
            buf.push_back(static_cast<uint8_t>(held));
          }
        },
        value);
  }
}

utils::Expected<vectors::Metadata, utils::Error> DecodeTypedMetadata(Reader& reader) {
  uint64_t count = 0;
  if (!reader.ReadLE(count, sizeof(uint32_t))) {
    return utils::MakeUnexpected(TruncatedError("METASET metadata count"));
  }
  if (count > reader.remaining()) {
    return utils::MakeUnexpected(
        utils::MakeError(utils::ErrorCode::kStorageCorrupted, "METASET metadata entry count exceeds payload"));
  }

  vectors::Metadata metadata;
  for (uint64_t i = 0; i < count; ++i) {
    std::string key;
    uint64_t tag = 0;
    if (!reader.ReadString(key) || !reader.ReadLE(tag, sizeof(uint8_t))) {
      return utils::MakeUnexpected(TruncatedError("METASET metadata entry"));
    }
    switch (static_cast<MetadataValueTag>(tag)) {
      case MetadataValueTag::kString: {
        std::string value;
        if (!reader.ReadString(value)) {
          return utils::MakeUnexpected(TruncatedError("METASET string value"));
        }
        metadata.emplace(std::move(key), std::move(value));
        break;
      }
      case MetadataValueTag::kInt64: {
        uint64_t raw = 0;
        if (!reader.ReadLE(raw, sizeof(int64_t))) {
          return utils::MakeUnexpected(TruncatedError("METASET integer value"));
        }
        metadata.emplace(std::move(key), static_cast<int64_t>(raw));
        break;
      }
      case MetadataValueTag::kDouble: {
        double value = 0.0;
        if (!reader.ReadDouble(value)) {
          return utils::MakeUnexpected(TruncatedError("METASET double value"));
        }
        metadata.emplace(std::move(key), value);
        break;
      }
      case MetadataValueTag::kBool: {
        uint64_t raw = 0;
        if (!reader.ReadLE(raw, sizeof(uint8_t)) || raw > 1) {
          return utils::MakeUnexpected(TruncatedError("METASET boolean value"));
        }
        metadata.emplace(std::move(key), raw == 1);
        break;
      }
      default:
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kStorageCorrupted, "METASET metadata value tag is invalid"));
    }
  }
  return metadata;
}

}  // namespace

storage::WalOpType WalOpForCommand(const Command& cmd) {
  switch (cmd.type) {
    case CommandType::kEvent:
      return cmd.event_type == EventType::DEL ? WalOpType::kEventDel : WalOpType::kEventAdd;
    case CommandType::kVecset:
      return WalOpType::kVecSet;
    case CommandType::kVecdel:
      return WalOpType::kVecDel;
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
      if (cmd.metadata.has_value()) {
        EncodeTypedMetadata(buf, *cmd.metadata);
      }
      break;
    }
    case CommandType::kVecdel: {
      // VECDEL: str id
      AppendString(buf, cmd.id);
      break;
    }
    case CommandType::kMetaset: {
      // METASET: str id, then either a legacy text filter or typed metadata.
      AppendString(buf, cmd.id);
      if (cmd.metadata.has_value()) {
        EncodeTypedMetadata(buf, *cmd.metadata);
      } else {
        AppendString(buf, cmd.filter_expr);
      }
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
      if (dim > static_cast<uint64_t>(std::numeric_limits<int>::max()) || dim > reader.remaining() / sizeof(float)) {
        return utils::MakeUnexpected(
            utils::MakeError(utils::ErrorCode::kStorageCorrupted, "VECSET dimension exceeds payload bounds"));
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
      if (reader.remaining() > 0) {
        uint64_t magic = 0;
        if (!reader.ReadLE(magic, sizeof(uint32_t)) || magic != kTypedMetadataMagic) {
          return utils::MakeUnexpected(
              utils::MakeError(utils::ErrorCode::kStorageCorrupted, "VECSET metadata extension is invalid"));
        }
        auto metadata = DecodeTypedMetadata(reader);
        if (!metadata) {
          return utils::MakeUnexpected(metadata.error());
        }
        cmd.metadata = std::move(*metadata);
      }
      break;
    }
    case WalOpType::kVecDel: {
      cmd.type = CommandType::kVecdel;
      if (!reader.ReadString(cmd.id)) {
        return utils::MakeUnexpected(TruncatedError("VECDEL id"));
      }
      break;
    }
    case WalOpType::kMetaSet: {
      cmd.type = CommandType::kMetaset;
      if (!reader.ReadString(cmd.id)) {
        return utils::MakeUnexpected(TruncatedError("METASET id"));
      }
      const size_t metadata_start = reader.position();
      uint64_t magic = 0;
      if (reader.remaining() >= sizeof(uint32_t) && reader.ReadLE(magic, sizeof(uint32_t)) &&
          magic == kTypedMetadataMagic) {
        auto metadata = DecodeTypedMetadata(reader);
        if (!metadata) {
          return utils::MakeUnexpected(metadata.error());
        }
        cmd.metadata = std::move(*metadata);
      } else {
        reader.set_position(metadata_start);
        if (!reader.ReadString(cmd.filter_expr)) {
          return utils::MakeUnexpected(TruncatedError("METASET filter_expr"));
        }
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

/**
 * @file id_validation.h
 * @brief Character-set rules shared by every identifier the event layer accepts
 */

#pragma once

#include <string>

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::events {

/**
 * @brief Reject an identifier the line protocol cannot carry
 *
 * Item ids are written unescaped into the count-framed TCP response and the
 * shipped clients decide a response is complete by counting raw newlines, so an
 * id carrying a control character, a newline or a space lets one ingestion
 * surface corrupt another surface's framing. Bytes at or above 0x80 are left
 * alone so multi-byte UTF-8 ids keep working.
 *
 * @param label Human-readable name of the field, used in the error message
 * @param value Identifier to check
 * @return Expected<void, Error> Success or the reason it was rejected
 */
inline utils::Expected<void, utils::Error> ValidateIdentifier(const std::string& label, const std::string& value) {
  if (value.empty()) {
    return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kEventStoreError, label + " cannot be empty"));
  }
  for (const char raw : value) {
    const auto byte = static_cast<unsigned char>(raw);
    constexpr unsigned char kSpace = 0x20;
    constexpr unsigned char kDelete = 0x7F;
    if (byte <= kSpace || byte == kDelete) {
      return utils::MakeUnexpected(utils::MakeError(utils::ErrorCode::kEventStoreError,
                                                    label + " must not contain whitespace or control characters"));
    }
  }
  return {};
}

}  // namespace nvecd::events

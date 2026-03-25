/**
 * @file md5_test.cpp
 * @brief Unit tests for MD5 implementation against RFC 1321 test vectors
 */

#include "cache/md5.h"

#include <gtest/gtest.h>

#include <iomanip>
#include <sstream>
#include <string>

using namespace nvecd::cache;

namespace {

/// Convert 16-byte digest to hex string
std::string DigestToHex(const uint8_t digest[16]) {  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (int i = 0; i < 16; ++i) {
    oss << std::setw(2)
        << static_cast<unsigned int>(digest[i]);  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
  }
  return oss.str();
}

}  // namespace

// RFC 1321 test vectors

TEST(MD5Test, EmptyString) {
  uint8_t digest[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  MD5::Hash("", digest);
  EXPECT_EQ(DigestToHex(digest), "d41d8cd98f00b204e9800998ecf8427e");
}

TEST(MD5Test, SingleCharA) {
  uint8_t digest[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  MD5::Hash("a", digest);
  EXPECT_EQ(DigestToHex(digest), "0cc175b9c0f1b6a831c399e269772661");
}

TEST(MD5Test, Abc) {
  uint8_t digest[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  MD5::Hash("abc", digest);
  EXPECT_EQ(DigestToHex(digest), "900150983cd24fb0d6963f7d28e17f72");
}

TEST(MD5Test, MessageDigest) {
  uint8_t digest[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  MD5::Hash("message digest", digest);
  EXPECT_EQ(DigestToHex(digest), "f96b697d7cb7938d525a2f31aaf161d0");
}

TEST(MD5Test, Alphabet) {
  uint8_t digest[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  MD5::Hash("abcdefghijklmnopqrstuvwxyz", digest);
  EXPECT_EQ(DigestToHex(digest), "c3fcd3d76192e4007dfb496cca67e13b");
}

TEST(MD5Test, AlphanumericMixed) {
  uint8_t digest[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  MD5::Hash(
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
      digest);
  EXPECT_EQ(DigestToHex(digest), "d174ab98d277d9f5a5611c2c9f419d9f");
}

TEST(MD5Test, Numeric) {
  uint8_t digest[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  MD5::Hash(
      "12345678901234567890123456789012345678901234567890123456789012345678901234"
      "567890",
      digest);
  EXPECT_EQ(DigestToHex(digest), "57edf4a22be3c955ac49da2e2107b67a");
}

// Incremental update test
TEST(MD5Test, IncrementalUpdate) {
  // Hash "abc" all at once
  uint8_t digest_single[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  MD5::Hash("abc", digest_single);

  // Hash "abc" incrementally
  uint8_t digest_incremental[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  MD5 md5;
  md5.Update("a");
  md5.Update("bc");
  md5.Finalize(digest_incremental);

  EXPECT_EQ(DigestToHex(digest_single), DigestToHex(digest_incremental));
}

// Determinism test
TEST(MD5Test, Deterministic) {
  uint8_t digest1[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  uint8_t digest2[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  MD5::Hash("deterministic test", digest1);
  MD5::Hash("deterministic test", digest2);
  EXPECT_EQ(DigestToHex(digest1), DigestToHex(digest2));
}

// Different inputs produce different digests
TEST(MD5Test, DifferentInputs) {
  uint8_t digest1[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  uint8_t digest2[16];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  MD5::Hash("hello", digest1);
  MD5::Hash("world", digest2);
  EXPECT_NE(DigestToHex(digest1), DigestToHex(digest2));
}

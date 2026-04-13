/**
 * @file string_utils_test.cpp
 * @brief Unit tests for string utility functions
 */

#include "utils/string_utils.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace nvecd::utils;

// ========== Utf8ToCodepoints tests ==========

TEST(Utf8ToCodepointsTest, AsciiString) {
  auto cps = Utf8ToCodepoints("abc");
  ASSERT_EQ(cps.size(), 3u);
  EXPECT_EQ(cps[0], 'a');
  EXPECT_EQ(cps[1], 'b');
  EXPECT_EQ(cps[2], 'c');
}

TEST(Utf8ToCodepointsTest, EmptyString) {
  auto cps = Utf8ToCodepoints("");
  EXPECT_TRUE(cps.empty());
}

TEST(Utf8ToCodepointsTest, TwoByteCharacters) {
  // U+00E9 LATIN SMALL LETTER E WITH ACUTE = 0xC3 0xA9
  auto cps = Utf8ToCodepoints("\xC3\xA9");
  ASSERT_EQ(cps.size(), 1u);
  EXPECT_EQ(cps[0], 0x00E9u);
}

TEST(Utf8ToCodepointsTest, ThreeByteCharacters) {
  // U+3042 HIRAGANA LETTER A = 0xE3 0x81 0x82
  auto cps = Utf8ToCodepoints("\xE3\x81\x82");
  ASSERT_EQ(cps.size(), 1u);
  EXPECT_EQ(cps[0], 0x3042u);
}

TEST(Utf8ToCodepointsTest, FourByteCharacters) {
  // U+1F600 GRINNING FACE = 0xF0 0x9F 0x98 0x80
  auto cps = Utf8ToCodepoints("\xF0\x9F\x98\x80");
  ASSERT_EQ(cps.size(), 1u);
  EXPECT_EQ(cps[0], 0x1F600u);
}

TEST(Utf8ToCodepointsTest, MixedContent) {
  // "a" + hiragana "a" (U+3042)
  auto cps = Utf8ToCodepoints("a\xE3\x81\x82");
  ASSERT_EQ(cps.size(), 2u);
  EXPECT_EQ(cps[0], 'a');
  EXPECT_EQ(cps[1], 0x3042u);
}

// ========== CodepointsToUtf8 tests ==========

TEST(CodepointsToUtf8Test, AsciiCodepoints) {
  std::vector<uint32_t> cps = {'H', 'i'};
  EXPECT_EQ(CodepointsToUtf8(cps), "Hi");
}

TEST(CodepointsToUtf8Test, EmptyVector) {
  std::vector<uint32_t> cps;
  EXPECT_EQ(CodepointsToUtf8(cps), "");
}

TEST(CodepointsToUtf8Test, RoundTrip) {
  std::string original = "Hello\xE4\xB8\x96\xE7\x95\x8C";  // "Hello世界"
  auto cps = Utf8ToCodepoints(original);
  std::string roundtrip = CodepointsToUtf8(cps);
  EXPECT_EQ(roundtrip, original);
}

// ========== NormalizeText tests (fallback path, no ICU) ==========

TEST(NormalizeTextTest, LowercaseConversion) {
  std::string result = NormalizeText("HELLO WORLD", false, "keep", true);
  EXPECT_EQ(result, "hello world");
}

TEST(NormalizeTextTest, NoConversion) {
  std::string result = NormalizeText("Hello", false, "keep", false);
  EXPECT_EQ(result, "Hello");
}

TEST(NormalizeTextTest, EmptyString) {
  std::string result = NormalizeText("", false, "keep", true);
  EXPECT_EQ(result, "");
}

TEST(NormalizeTextTest, MixedCase) {
  std::string result = NormalizeText("HeLLo WoRLD", false, "keep", true);
  EXPECT_EQ(result, "hello world");
}

// ========== GenerateNgrams tests ==========

TEST(GenerateNgramsTest, Unigrams) {
  auto ngrams = GenerateNgrams("abc", 1);
  ASSERT_EQ(ngrams.size(), 3u);
  EXPECT_EQ(ngrams[0], "a");
  EXPECT_EQ(ngrams[1], "b");
  EXPECT_EQ(ngrams[2], "c");
}

TEST(GenerateNgramsTest, Bigrams) {
  auto ngrams = GenerateNgrams("abcd", 2);
  ASSERT_EQ(ngrams.size(), 3u);
  EXPECT_EQ(ngrams[0], "ab");
  EXPECT_EQ(ngrams[1], "bc");
  EXPECT_EQ(ngrams[2], "cd");
}

TEST(GenerateNgramsTest, EmptyString) {
  auto ngrams = GenerateNgrams("", 1);
  EXPECT_TRUE(ngrams.empty());
}

TEST(GenerateNgramsTest, NgramSizeZero) {
  auto ngrams = GenerateNgrams("abc", 0);
  EXPECT_TRUE(ngrams.empty());
}

TEST(GenerateNgramsTest, NgramSizeLargerThanInput) {
  auto ngrams = GenerateNgrams("ab", 3);
  EXPECT_TRUE(ngrams.empty());
}

TEST(GenerateNgramsTest, UnicodeUnigrams) {
  // Three CJK characters: U+4E16 U+754C U+4EBA ("世界人")
  auto ngrams = GenerateNgrams("\xE4\xB8\x96\xE7\x95\x8C\xE4\xBA\xBA", 1);
  ASSERT_EQ(ngrams.size(), 3u);
  EXPECT_EQ(ngrams[0], "\xE4\xB8\x96");  // 世
  EXPECT_EQ(ngrams[1], "\xE7\x95\x8C");  // 界
  EXPECT_EQ(ngrams[2], "\xE4\xBA\xBA");  // 人
}

// ========== GenerateHybridNgrams tests ==========

TEST(GenerateHybridNgramsTest, AsciiOnly) {
  auto ngrams = GenerateHybridNgrams("abcd", 2, 1);
  ASSERT_EQ(ngrams.size(), 3u);
  EXPECT_EQ(ngrams[0], "ab");
  EXPECT_EQ(ngrams[1], "bc");
  EXPECT_EQ(ngrams[2], "cd");
}

TEST(GenerateHybridNgramsTest, CjkOnly) {
  // Three CJK characters with kanji_ngram_size=1
  auto ngrams = GenerateHybridNgrams("\xE4\xB8\x96\xE7\x95\x8C\xE4\xBA\xBA", 2, 1);
  ASSERT_EQ(ngrams.size(), 3u);
  EXPECT_EQ(ngrams[0], "\xE4\xB8\x96");  // 世
  EXPECT_EQ(ngrams[1], "\xE7\x95\x8C");  // 界
  EXPECT_EQ(ngrams[2], "\xE4\xBA\xBA");  // 人
}

TEST(GenerateHybridNgramsTest, EmptyString) {
  auto ngrams = GenerateHybridNgrams("", 2, 1);
  EXPECT_TRUE(ngrams.empty());
}

// ========== FormatBytes tests ==========

TEST(FormatBytesTest, ZeroBytes) {
  EXPECT_EQ(FormatBytes(0), "0B");
}

TEST(FormatBytesTest, ByteRange) {
  // 500 >= 100, so precision is 0 decimal places
  EXPECT_EQ(FormatBytes(500), "500B");
}

TEST(FormatBytesTest, SmallByteRange) {
  // 5 < 10, so precision is 2 decimal places
  EXPECT_EQ(FormatBytes(5), "5.00B");
}

TEST(FormatBytesTest, KilobyteRange) {
  EXPECT_EQ(FormatBytes(1024), "1.00KB");
}

TEST(FormatBytesTest, MegabyteRange) {
  EXPECT_EQ(FormatBytes(1024 * 1024), "1.00MB");
}

TEST(FormatBytesTest, GigabyteRange) {
  size_t one_gb = 1024UL * 1024UL * 1024UL;
  EXPECT_EQ(FormatBytes(one_gb), "1.00GB");
}

TEST(FormatBytesTest, LargeValuePrecision) {
  // 150 MB = 150.0 MB (>= 100, so 0 decimal places)
  size_t bytes = 150UL * 1024UL * 1024UL;
  EXPECT_EQ(FormatBytes(bytes), "150MB");
}

TEST(FormatBytesTest, MediumValuePrecision) {
  // 15 MB (>= 10, so 1 decimal place)
  size_t bytes = 15UL * 1024UL * 1024UL;
  EXPECT_EQ(FormatBytes(bytes), "15.0MB");
}

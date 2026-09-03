#include "utils/structured_log.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>

namespace nvecd::utils {
namespace {

class StructuredLogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    previous_logger_ = spdlog::default_logger();
    sink_ = std::make_shared<spdlog::sinks::ostream_sink_mt>(output_);
    logger_ = std::make_shared<spdlog::logger>("structured-log-test", sink_);
    logger_->set_pattern("%v");
    spdlog::set_default_logger(logger_);
  }

  void TearDown() override {
    spdlog::set_default_logger(previous_logger_);
    StructuredLog::SetFormat(LogFormat::JSON);
  }

  std::ostringstream output_;
  std::shared_ptr<spdlog::sinks::ostream_sink_mt> sink_;
  std::shared_ptr<spdlog::logger> logger_;
  std::shared_ptr<spdlog::logger> previous_logger_;
};

TEST_F(StructuredLogTest, ParseErrorsLogOnlyTheCommandVerb) {
  LogCommandParseError("AUTH super-secret\r\x01", "malformed");
  logger_->flush();
  const std::string logged = output_.str();
  EXPECT_NE(logged.find("AUTH"), std::string::npos);
  EXPECT_EQ(logged.find("super-secret"), std::string::npos);
}

TEST_F(StructuredLogTest, TextFieldsQuoteAndEscapeAllControlAndBackslashCharacters) {
  StructuredLog::SetFormat(LogFormat::TEXT);
  StructuredLog().Event("escape").Field("value", std::string("a\rb\tc\\d")).Info();
  logger_->flush();
  EXPECT_NE(output_.str().find("value=\"a\\rb\\tc\\\\d\""), std::string::npos);
}

TEST_F(StructuredLogTest, JsonFieldsEscapeQuotesBackslashesAndShorthandControlCharacters) {
  StructuredLog().Event("escape").Field("value", std::string("q\"b\\n\nt\tr\rf\fbs\b")).Info();
  logger_->flush();
  const std::string logged = output_.str();
  EXPECT_NE(logged.find(R"("value":"q\"b\\n\nt\tr\rf\fbs\b")"), std::string::npos) << logged;
  // No raw control byte may survive into the payload: an unescaped newline
  // would split one record into two log lines, and the sink appends exactly
  // one newline of its own.
  EXPECT_EQ(std::count(logged.begin(), logged.end(), '\n'), 1);
  EXPECT_EQ(logged.find('\t'), std::string::npos);
  EXPECT_EQ(logged.find('\r'), std::string::npos);
}

TEST_F(StructuredLogTest, JsonEscapesRemainingControlBytesAsUnicodeEscapes) {
  StructuredLog().Event("escape").Field("value", std::string("\x01\x0e\x1f")).Info();
  logger_->flush();
  const std::string logged = output_.str();
  EXPECT_NE(logged.find(R"("value":"\u0001\u000e\u001f")"), std::string::npos) << logged;
}

TEST_F(StructuredLogTest, JsonPassesMultiByteUtf8BytesThroughUnescaped) {
  // Bytes at or above 0x80 are not control characters and must be emitted
  // verbatim so multi-byte UTF-8 survives the JSON encoding intact.
  const std::string value = "\xe6\x97\xa5\xc3\xa9";
  StructuredLog().Event("escape").Field("value", value).Info();
  logger_->flush();
  const std::string logged = output_.str();
  EXPECT_NE(logged.find("\"value\":\"" + value + "\""), std::string::npos) << logged;
  EXPECT_EQ(logged.find("\\u00"), std::string::npos) << logged;
}

TEST_F(StructuredLogTest, JsonEscapesEventAndMessageFields) {
  // The event name and message go through the same escaper as field values;
  // an unescaped quote there would break the surrounding JSON object.
  StructuredLog().Event("ev\"ent").Message("mes\\sage\nline").Info();
  logger_->flush();
  const std::string logged = output_.str();
  EXPECT_NE(logged.find(R"({"event":"ev\"ent","message":"mes\\sage\nline"})"), std::string::npos) << logged;
}

}  // namespace
}  // namespace nvecd::utils

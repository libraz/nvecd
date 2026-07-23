#include "utils/structured_log.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

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

}  // namespace
}  // namespace nvecd::utils

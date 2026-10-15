#include "utils/log_level.h"

#include <gtest/gtest.h>

namespace nvecd::utils {
namespace {

TEST(LogLevelTest, MapsEveryConfiguredName) {
  EXPECT_EQ(ParseLogLevel("trace"), spdlog::level::trace);
  EXPECT_EQ(ParseLogLevel("debug"), spdlog::level::debug);
  EXPECT_EQ(ParseLogLevel("info"), spdlog::level::info);
  EXPECT_EQ(ParseLogLevel("warn"), spdlog::level::warn);
  EXPECT_EQ(ParseLogLevel("error"), spdlog::level::err);
}

TEST(LogLevelTest, RejectsEverythingElse) {
  EXPECT_FALSE(ParseLogLevel(""));
  EXPECT_FALSE(ParseLogLevel("INFO"));
  EXPECT_FALSE(ParseLogLevel("warning"));
  EXPECT_FALSE(ParseLogLevel("err"));
  EXPECT_FALSE(ParseLogLevel("critical"));
  EXPECT_FALSE(ParseLogLevel("off"));
}

}  // namespace
}  // namespace nvecd::utils

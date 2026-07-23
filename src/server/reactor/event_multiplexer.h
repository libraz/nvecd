/**
 * @file event_multiplexer.h
 * @brief Portable, level-triggered readiness notification for the TCP reactor.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::server::reactor {

using RegistrationToken = uint64_t;

namespace event {
constexpr uint8_t kNone = 0;
constexpr uint8_t kReadable = 1U << 0;
constexpr uint8_t kWritable = 1U << 1;
constexpr uint8_t kError = 1U << 2;
constexpr uint8_t kHangup = 1U << 3;
}  // namespace event

struct ReadyEvent {
  int fd = -1;
  uint8_t events = event::kNone;
  RegistrationToken token = 0;
};

/**
 * The reactor owns fd lifetime; a multiplexer only observes readiness.
 * All implementations use level-triggered events so a bounded reactor turn
 * cannot lose unread data.
 */
class EventMultiplexer {
 public:
  virtual ~EventMultiplexer() = default;
  EventMultiplexer(const EventMultiplexer&) = delete;
  EventMultiplexer& operator=(const EventMultiplexer&) = delete;

  virtual utils::Expected<void, utils::Error> Open() = 0;
  virtual utils::Expected<void, utils::Error> Add(int fd, uint8_t interest, RegistrationToken token) = 0;
  virtual utils::Expected<void, utils::Error> Modify(int fd, uint8_t interest, RegistrationToken token) = 0;
  virtual utils::Expected<void, utils::Error> Remove(int fd) = 0;
  virtual utils::Expected<void, utils::Error> Poll(int timeout_ms, std::vector<ReadyEvent>& ready) = 0;
  virtual utils::Expected<void, utils::Error> Wake() = 0;
  virtual const char* Name() const = 0;

 protected:
  EventMultiplexer() = default;
};

std::unique_ptr<EventMultiplexer> CreateEventMultiplexer();

}  // namespace nvecd::server::reactor

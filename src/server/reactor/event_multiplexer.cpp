/**
 * @file event_multiplexer.cpp
 * @brief epoll/kqueue backends for EventMultiplexer.
 */

#include "server/reactor/event_multiplexer.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#include <sys/time.h>
#endif

namespace nvecd::server::reactor {
namespace {

utils::Expected<void, utils::Error> SyscallError(const char* operation) {
  return utils::MakeUnexpected(
      utils::MakeError(utils::ErrorCode::kIOError, std::string(operation) + ": " + std::strerror(errno)));
}

#if defined(__linux__)
class EpollMultiplexer final : public EventMultiplexer {
 public:
  ~EpollMultiplexer() override {
    if (wake_fd_ >= 0) {
      ::close(wake_fd_);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  utils::Expected<void, utils::Error> Open() override {
    fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd_ < 0) {
      return SyscallError("epoll_create1");
    }
    wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0) {
      return SyscallError("eventfd");
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = 0;
    if (::epoll_ctl(fd_, EPOLL_CTL_ADD, wake_fd_, &event) != 0) {
      return SyscallError("epoll_ctl wake add");
    }
    events_.resize(64);
    return {};
  }

  utils::Expected<void, utils::Error> Add(int client_fd, uint8_t interest, RegistrationToken token) override {
    return Control(EPOLL_CTL_ADD, client_fd, interest, token);
  }
  utils::Expected<void, utils::Error> Modify(int client_fd, uint8_t interest, RegistrationToken token) override {
    return Control(EPOLL_CTL_MOD, client_fd, interest, token);
  }
  utils::Expected<void, utils::Error> Remove(int client_fd) override {
    if (::epoll_ctl(fd_, EPOLL_CTL_DEL, client_fd, nullptr) != 0 && errno != ENOENT && errno != EBADF) {
      return SyscallError("epoll_ctl del");
    }
    return {};
  }

  utils::Expected<void, utils::Error> Poll(int timeout_ms, std::vector<ReadyEvent>& ready) override {
    ready.clear();
    const int count = ::epoll_wait(fd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
    if (count < 0) {
      if (errno == EINTR) {
        return {};
      }
      return SyscallError("epoll_wait");
    }
    for (int i = 0; i < count; ++i) {
      const auto& item = events_[static_cast<size_t>(i)];
      if (item.data.u64 == 0) {
        uint64_t value = 0;
        while (::read(wake_fd_, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))) {
        }
        continue;
      }
      uint8_t flags = event::kNone;
      if ((item.events & EPOLLIN) != 0U)
        flags |= event::kReadable;
      if ((item.events & EPOLLOUT) != 0U)
        flags |= event::kWritable;
      if ((item.events & EPOLLERR) != 0U)
        flags |= event::kError;
      if ((item.events & (EPOLLHUP | EPOLLRDHUP)) != 0U)
        flags |= event::kHangup;
      ready.push_back({-1, flags, item.data.u64});
    }
    if (static_cast<size_t>(count) == events_.size() && events_.size() < 4096) {
      events_.resize(events_.size() * 2);
    }
    return {};
  }

  utils::Expected<void, utils::Error> Wake() override {
    const uint64_t value = 1;
    if (::write(wake_fd_, &value, sizeof(value)) < 0 && errno != EAGAIN) {
      return SyscallError("eventfd write");
    }
    return {};
  }
  const char* Name() const override { return "epoll"; }

 private:
  utils::Expected<void, utils::Error> Control(int operation, int client_fd, uint8_t interest, RegistrationToken token) {
    epoll_event event{};
    event.events = EPOLLRDHUP | EPOLLHUP | EPOLLERR;
    if ((interest & event::kReadable) != 0U)
      event.events |= EPOLLIN;
    if ((interest & event::kWritable) != 0U)
      event.events |= EPOLLOUT;
    event.data.u64 = token;
    if (::epoll_ctl(fd_, operation, client_fd, &event) != 0) {
      return SyscallError("epoll_ctl");
    }
    return {};
  }

  int fd_ = -1;
  int wake_fd_ = -1;
  std::vector<epoll_event> events_;
};
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
class KqueueMultiplexer final : public EventMultiplexer {
 public:
  ~KqueueMultiplexer() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  utils::Expected<void, utils::Error> Open() override {
    fd_ = ::kqueue();
    if (fd_ < 0) {
      return SyscallError("kqueue");
    }
    struct kevent event {};
    EV_SET(&event, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(fd_, &event, 1, nullptr, 0, nullptr) < 0) {
      return SyscallError("kevent wake add");
    }
    events_.resize(64);
    return {};
  }

  utils::Expected<void, utils::Error> Add(int client_fd, uint8_t interest, RegistrationToken token) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (interest_.find(client_fd) != interest_.end()) {
      errno = EEXIST;
      return SyscallError("kqueue add duplicate");
    }
    if (auto result = Apply(client_fd, event::kNone, interest, token); !result)
      return result;
    interest_[client_fd] = interest;
    return {};
  }
  utils::Expected<void, utils::Error> Modify(int client_fd, uint8_t interest, RegistrationToken token) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = interest_.find(client_fd);
    if (found == interest_.end()) {
      errno = ENOENT;
      return SyscallError("kqueue modify unknown fd");
    }
    if (auto result = Apply(client_fd, found->second, interest, token); !result)
      return result;
    found->second = interest;
    return {};
  }
  utils::Expected<void, utils::Error> Remove(int client_fd) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = interest_.find(client_fd);
    if (found == interest_.end())
      return {};
    if (auto result = Apply(client_fd, found->second, event::kNone, 0); !result)
      return result;
    interest_.erase(found);
    return {};
  }

  utils::Expected<void, utils::Error> Poll(int timeout_ms, std::vector<ReadyEvent>& ready) override {
    ready.clear();
    timespec timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000 * 1000};
    const int count = ::kevent(fd_, nullptr, 0, events_.data(), static_cast<int>(events_.size()), &timeout);
    if (count < 0) {
      if (errno == EINTR)
        return {};
      return SyscallError("kevent wait");
    }
    for (int i = 0; i < count; ++i) {
      const auto& item = events_[static_cast<size_t>(i)];
      if (item.filter == EVFILT_USER && item.ident == kWakeIdent)
        continue;
      uint8_t flags = item.filter == EVFILT_READ ? event::kReadable : event::kWritable;
      if ((item.flags & EV_ERROR) != 0U)
        flags |= event::kError;
      if ((item.flags & EV_EOF) != 0U)
        flags |= event::kHangup;
      ready.push_back({static_cast<int>(item.ident), flags,
                       static_cast<RegistrationToken>(reinterpret_cast<uintptr_t>(item.udata))});
    }
    if (static_cast<size_t>(count) == events_.size() && events_.size() < 4096)
      events_.resize(events_.size() * 2);
    return {};
  }

  utils::Expected<void, utils::Error> Wake() override {
    struct kevent event {};
    EV_SET(&event, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    if (::kevent(fd_, &event, 1, nullptr, 0, nullptr) < 0)
      return SyscallError("kevent wake");
    return {};
  }
  const char* Name() const override { return "kqueue"; }

 private:
  utils::Expected<void, utils::Error> Apply(int client_fd, uint8_t before, uint8_t after, RegistrationToken token) {
    struct kevent changes[2]{};
    int count = 0;
    const auto change = [&](short filter, uint8_t flag) {
      if ((before & flag) == (after & flag))
        return;
      EV_SET(&changes[count++], static_cast<uintptr_t>(client_fd), filter,
             (after & flag) != 0U ? EV_ADD | EV_ENABLE : EV_DELETE, 0, 0,
             reinterpret_cast<void*>(static_cast<uintptr_t>(token)));
    };
    change(EVFILT_READ, event::kReadable);
    change(EVFILT_WRITE, event::kWritable);
    if (count > 0 && ::kevent(fd_, changes, count, nullptr, 0, nullptr) < 0 && errno != ENOENT)
      return SyscallError("kevent control");
    return {};
  }
  static constexpr uintptr_t kWakeIdent = 1;
  int fd_ = -1;
  std::mutex mutex_;
  std::unordered_map<int, uint8_t> interest_;
  std::vector<struct kevent> events_;
};
#endif
}  // namespace

std::unique_ptr<EventMultiplexer> CreateEventMultiplexer() {
#if defined(__linux__)
  return std::make_unique<EpollMultiplexer>();
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  return std::make_unique<KqueueMultiplexer>();
#else
  return nullptr;
#endif
}

}  // namespace nvecd::server::reactor

/**
 * @file decay_scheduler.cpp
 * @brief Implementation of DecayScheduler
 */

#include "server/decay_scheduler.h"

#include <chrono>
#include <utility>

#include "utils/structured_log.h"

namespace nvecd::server {

namespace {
constexpr int kCheckIntervalMs = 1000;  // Check for shutdown every second
constexpr int kPruneEveryNCycles = 10;  // Prune once per this many decay cycles
}  // namespace

DecayScheduler::DecayScheduler(events::CoOccurrenceIndex* co_index, int interval_sec, double alpha,
                               MaintenanceCallback maintenance_callback)
    : co_index_(co_index),
      interval_sec_(interval_sec),
      alpha_(alpha),
      maintenance_callback_(std::move(maintenance_callback)) {}

DecayScheduler::~DecayScheduler() {
  Stop();
}

void DecayScheduler::Start() {
  if (running_) {
    utils::StructuredLog()
        .Event("server_warning")
        .Field("component", "decay_scheduler")
        .Field("type", "already_running")
        .Warn();
    return;
  }

  if (interval_sec_ <= 0) {
    utils::StructuredLog().Event("decay_scheduler_disabled").Field("reason", "interval_sec <= 0").Info();
    return;
  }

  if (co_index_ == nullptr) {
    utils::StructuredLog().Event("decay_scheduler_disabled").Field("reason", "co_index is null").Info();
    return;
  }

  utils::StructuredLog()
      .Event("decay_scheduler_starting")
      .Field("interval_sec", static_cast<uint64_t>(interval_sec_))
      .Field("alpha", alpha_)
      .Info();

  running_ = true;
  scheduler_thread_ = std::make_unique<std::thread>(&DecayScheduler::SchedulerLoop, this);
}

void DecayScheduler::Stop() {
  if (!running_) {
    return;
  }

  utils::StructuredLog().Event("decay_scheduler_stopping").Info();
  running_ = false;

  if (scheduler_thread_ && scheduler_thread_->joinable()) {
    scheduler_thread_->join();
  }

  utils::StructuredLog().Event("decay_scheduler_stopped").Info();
}

void DecayScheduler::SchedulerLoop() {
  utils::StructuredLog().Event("decay_scheduler_thread_started").Info();

  // Calculate next decay time
  auto next_decay_time = std::chrono::steady_clock::now() + std::chrono::seconds(interval_sec_);
  int cycles_since_prune = 0;

  while (running_) {
    auto now = std::chrono::steady_clock::now();

    // Check if it's time to decay
    if (now >= next_decay_time) {
      const bool prune = ++cycles_since_prune >= kPruneEveryNCycles;
      utils::Expected<void, utils::Error> maintained;
      if (maintenance_callback_) {
        maintained = maintenance_callback_(alpha_, prune);
      } else {
        co_index_->ApplyDecay(alpha_);
        if (prune) {
          co_index_->Prune();
        }
      }
      if (!maintained) {
        utils::StructuredLog().Event("decay_maintenance_failed").Field("error", maintained.error().message()).Error();
      }
      if (prune) {
        cycles_since_prune = 0;
      }

      // Schedule next decay
      next_decay_time = std::chrono::steady_clock::now() + std::chrono::seconds(interval_sec_);
    }

    // Sleep for check interval
    std::this_thread::sleep_for(std::chrono::milliseconds(kCheckIntervalMs));
  }

  utils::StructuredLog().Event("decay_scheduler_thread_exiting").Info();
}

}  // namespace nvecd::server

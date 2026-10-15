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
  worker_.Start(std::chrono::seconds(interval_sec_), [this]() { RunCycle(); });
}

void DecayScheduler::Stop() {
  if (!running_) {
    return;
  }

  utils::StructuredLog().Event("decay_scheduler_stopping").Info();
  running_ = false;

  worker_.Stop();

  utils::StructuredLog().Event("decay_scheduler_stopped").Info();
}

void DecayScheduler::RunCycle() {
  const bool prune = ++cycles_since_prune_ >= kPruneEveryNCycles;
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
  } else if (prune) {
    // A refused pass leaves the counter high so the next cycle retries the prune.
    cycles_since_prune_ = 0;
  }
}

}  // namespace nvecd::server

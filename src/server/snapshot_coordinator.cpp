/**
 * @file snapshot_coordinator.cpp
 * @brief Snapshot session admission and request exclusion
 */

#include "server/snapshot_coordinator.h"

#include "storage/snapshot_fork.h"

namespace nvecd::server {

SnapshotCoordinator::SnapshotCoordinator(std::atomic<bool>& loading, std::atomic<bool>& read_only)
    : loading_(loading), read_only_(read_only) {}

SnapshotCoordinator::AccessGuard SnapshotCoordinator::AcquireStoreAccess(StoreAccess access) const {
  return AccessGuard(access == StoreAccess::kWrite ? write_gate_ : search_gate_);
}

utils::Expected<void, utils::Error> SnapshotCoordinator::AdmitLocked(SessionKind kind,
                                                                     storage::ForkSnapshotWriter* fork_writer) const {
  const auto refuse = [](utils::ErrorCode code, const char* message) -> utils::Expected<void, utils::Error> {
    return utils::MakeUnexpected(utils::MakeError(code, message));
  };
  if (shutting_down_) {
    return refuse(utils::ErrorCode::kCancelled, "Server is shutting down; snapshot operations are refused");
  }
  if (loading_.load(std::memory_order_acquire)) {
    return refuse(utils::ErrorCode::kSnapshotAlreadyInProgress,
                  kind == SessionKind::kLoad ? "Another snapshot load is already in progress"
                                             : "Cannot save snapshot while a snapshot load is in progress");
  }
  if (read_only_.load(std::memory_order_acquire)) {
    return refuse(utils::ErrorCode::kSnapshotAlreadyInProgress, kind == SessionKind::kLoad
                                                                    ? "A snapshot save is already in progress"
                                                                    : "Another snapshot save is already in progress");
  }
  // A brief session starts no child of its own beside one already running: the
  // fork writer refuses a second child, and retention runs only after a reap.
  if (kind != SessionKind::kBrief && fork_writer != nullptr) {
    fork_writer->CheckChild();
    if (fork_writer->IsInProgress()) {
      return refuse(utils::ErrorCode::kSnapshotAlreadyInProgress,
                    kind == SessionKind::kLoad ? "Cannot load snapshot while a background save is in progress"
                                               : "Another snapshot save is already in progress");
    }
  }
  return {};
}

utils::Expected<SnapshotCoordinator::LockSave, utils::Error> SnapshotCoordinator::BeginLockSave(
    storage::ForkSnapshotWriter* fork_writer) {
  {
    std::lock_guard admission(admission_mutex_);
    auto admitted = AdmitLocked(SessionKind::kLockSave, fork_writer);
    if (!admitted) {
      return utils::MakeUnexpected(admitted.error());
    }
    read_only_.store(true, std::memory_order_release);
  }
  // Writers arriving from here on are refused on read_only; the exclusive
  // acquisition waits only for those already admitted.
  return LockSave(read_only_, std::unique_lock(write_gate_));
}

utils::Expected<SnapshotCoordinator::Load, utils::Error> SnapshotCoordinator::BeginLoad(
    storage::ForkSnapshotWriter* fork_writer) {
  std::lock_guard admission(admission_mutex_);
  auto admitted = AdmitLocked(SessionKind::kLoad, fork_writer);
  if (!admitted) {
    return utils::MakeUnexpected(admitted.error());
  }
  loading_.store(true, std::memory_order_release);
  return Load(*this);
}

utils::Expected<SnapshotCoordinator::BriefSession, utils::Error> SnapshotCoordinator::BeginBriefSession() {
  std::unique_lock admission(admission_mutex_);
  auto admitted = AdmitLocked(SessionKind::kBrief, nullptr);
  if (!admitted) {
    return utils::MakeUnexpected(admitted.error());
  }
  return BriefSession(std::move(admission));
}

void SnapshotCoordinator::BeginShutdown() {
  std::lock_guard admission(admission_mutex_);
  shutting_down_ = true;
}

SnapshotCoordinator::LockSave::LockSave(std::atomic<bool>& read_only, std::unique_lock<std::shared_mutex> writes)
    : read_only_(&read_only), writes_(std::move(writes)) {}

SnapshotCoordinator::LockSave::LockSave(LockSave&& other) noexcept
    : read_only_(other.read_only_), writes_(std::move(other.writes_)) {
  other.read_only_ = nullptr;
}

SnapshotCoordinator::LockSave::~LockSave() {
  if (writes_.owns_lock()) {
    writes_.unlock();
  }
  if (read_only_ != nullptr) {
    read_only_->store(false, std::memory_order_release);
  }
}

SnapshotCoordinator::Load::Load(Load&& other) noexcept : owner_(other.owner_), keep_loading_(other.keep_loading_) {
  other.owner_ = nullptr;
}

SnapshotCoordinator::Load::~Load() {
  if (owner_ != nullptr && !keep_loading_) {
    owner_->loading_.store(false, std::memory_order_release);
  }
}

SnapshotCoordinator::Load::Publication SnapshotCoordinator::Load::Publish() {
  Publication publication;
  publication.writes = std::unique_lock(owner_->write_gate_);
  publication.searches = std::unique_lock(owner_->search_gate_);
  return publication;
}

}  // namespace nvecd::server

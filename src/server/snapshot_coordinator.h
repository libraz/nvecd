/**
 * @file snapshot_coordinator.h
 * @brief Admission, exclusion and shutdown drain for every snapshot writer
 *
 * Lock-mode DUMP SAVE, DUMP LOAD and brief sessions (a fork capture until its
 * child exists, retention cleanup) are snapshot sessions. At most one is
 * admitted at a time, a running fork child counts as one, and none is admitted
 * once shutdown has begun. A session exists only as a token this class issues,
 * so no writer can reach the save without passing admission.
 *
 * Request traffic is held off through two gates. Store writes hold the write
 * gate shared and searches the search gate. A lock-mode save holds only the
 * write gate exclusively, so searches keep running against the stores it
 * serializes; DUMP LOAD publication holds both.
 *
 * Lock order: write gate, search gate, HandlerContext::write_serialization_gate.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include "utils/error.h"
#include "utils/expected.h"

namespace nvecd::storage {
class ForkSnapshotWriter;
}  // namespace nvecd::storage

namespace nvecd::server {

/// Which request traffic a store access belongs to.
enum class StoreAccess : uint8_t {
  kWrite,   ///< EVENT, VECSET, VECDEL, METASET and co-occurrence maintenance
  kSearch,  ///< SIM and SIMV
};

/**
 * @brief Single owner of snapshot session admission and request exclusion
 *
 * The loading and read-only flags it publishes stay readable by the request
 * surfaces, which refuse traffic on them before waiting on a gate; only the
 * coordinator sets or clears them.
 */
class SnapshotCoordinator {
 public:
  /// Shared hold a store write or search keeps for its whole execution.
  using AccessGuard = std::shared_lock<std::shared_mutex>;

  /// Lock-mode save session: read_only is set and store writes are drained while it lives.
  class LockSave {
   public:
    LockSave(LockSave&& other) noexcept;
    LockSave& operator=(LockSave&&) = delete;
    LockSave(const LockSave&) = delete;
    LockSave& operator=(const LockSave&) = delete;
    ~LockSave();

   private:
    friend SnapshotCoordinator;
    LockSave(std::atomic<bool>& read_only, std::unique_lock<std::shared_mutex> writes);

    std::atomic<bool>* read_only_;
    std::unique_lock<std::shared_mutex> writes_;
  };

  /// DUMP LOAD session: loading is set while it lives, unless fail-stop keeps it set.
  class Load {
   public:
    /// Exclusive hold on store writes and searches for the publication phase.
    struct Publication {
      std::unique_lock<std::shared_mutex> writes;
      std::unique_lock<std::shared_mutex> searches;
    };

    Load(Load&& other) noexcept;
    Load& operator=(Load&&) = delete;
    Load(const Load&) = delete;
    Load& operator=(const Load&) = delete;
    ~Load();

    /// Drain store writes and searches so the staged state can be swapped in.
    [[nodiscard]] Publication Publish();

    /// Keep loading set after release: the server must not resume on this state.
    void KeepLoading() { keep_loading_ = true; }

   private:
    friend SnapshotCoordinator;
    explicit Load(SnapshotCoordinator& owner) : owner_(&owner) {}

    SnapshotCoordinator* owner_;
    bool keep_loading_ = false;
  };

  /// Brief session: admission stays held, so no other session starts while it lives.
  class BriefSession {
   public:
    BriefSession(BriefSession&&) noexcept = default;
    BriefSession& operator=(BriefSession&&) = delete;
    BriefSession(const BriefSession&) = delete;
    BriefSession& operator=(const BriefSession&) = delete;
    ~BriefSession() = default;

   private:
    friend SnapshotCoordinator;
    explicit BriefSession(std::unique_lock<std::mutex> admission) : admission_(std::move(admission)) {}

    std::unique_lock<std::mutex> admission_;
  };

  /**
   * @param loading Flag set for the whole of a DUMP LOAD
   * @param read_only Flag set for the whole of a lock-mode save
   */
  SnapshotCoordinator(std::atomic<bool>& loading, std::atomic<bool>& read_only);

  SnapshotCoordinator(const SnapshotCoordinator&) = delete;
  SnapshotCoordinator& operator=(const SnapshotCoordinator&) = delete;
  SnapshotCoordinator(SnapshotCoordinator&&) = delete;
  SnapshotCoordinator& operator=(SnapshotCoordinator&&) = delete;
  ~SnapshotCoordinator() = default;

  /**
   * @brief Hold off the sessions that exclude @p access for the guard's lifetime
   *
   * Callers refuse on the published flags first and re-check them once this
   * returns, so a request never mutates across a session boundary.
   */
  [[nodiscard]] AccessGuard AcquireStoreAccess(StoreAccess access) const;

  /**
   * @brief Admit a lock-mode save and drain in-flight store writes
   *
   * Publishes read_only, then waits for every admitted writer to leave.
   * Searches are not drained and keep being served.
   *
   * @param fork_writer Fork writer whose running child refuses the save (may be null)
   */
  utils::Expected<LockSave, utils::Error> BeginLockSave(storage::ForkSnapshotWriter* fork_writer);

  /**
   * @brief Admit a DUMP LOAD
   *
   * Publishes loading. No fork child can start until the token is released,
   * so the fork check made here is final for the whole load.
   *
   * @param fork_writer Fork writer whose running child refuses the load (may be null)
   */
  utils::Expected<Load, utils::Error> BeginLoad(storage::ForkSnapshotWriter* fork_writer);

  /**
   * @brief Admit a session that runs entirely while the token is held
   *
   * Used for a fork capture, whose child then counts as the running session,
   * and for retention cleanup. The token holds admission itself, so it must
   * stay short.
   */
  utils::Expected<BriefSession, utils::Error> BeginBriefSession();

  /**
   * @brief Refuse every later session
   *
   * Returns once any brief session in flight has released admission, so a
   * fork child started before this call is visible to the caller's wait.
   */
  void BeginShutdown();

 private:
  enum class SessionKind : uint8_t { kLockSave, kLoad, kBrief };

  /// The refusal for a new @p kind session, or success; called with admission held.
  utils::Expected<void, utils::Error> AdmitLocked(SessionKind kind, storage::ForkSnapshotWriter* fork_writer) const;

  std::atomic<bool>& loading_;
  std::atomic<bool>& read_only_;
  mutable std::shared_mutex write_gate_;
  mutable std::shared_mutex search_gate_;
  std::mutex admission_mutex_;
  bool shutting_down_ = false;  ///< Guarded by admission_mutex_
};

}  // namespace nvecd::server

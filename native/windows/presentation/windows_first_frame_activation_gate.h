#pragma once

#include <atomic>
#include <cstdint>

namespace vr {

// Keeps the Flutter viewport opaque until the Windows compositor has both the
// final initial viewport geometry and a successfully composited target from
// the current ring. Session tokens prevent a late callback from a retired
// player from activating a newly created player.
class WindowsFirstFrameActivationGate final {
 public:
  using Session = uint64_t;

  Session begin_session() {
    const Session session =
        session_.fetch_add(1, std::memory_order_acq_rel) + 1;
    policy_ready_.store(false, std::memory_order_release);
    initial_viewport_committed_.store(false, std::memory_order_release);
    active_.store(false, std::memory_order_release);
    return session;
  }

  void cancel_session() {
    session_.fetch_add(1, std::memory_order_acq_rel);
    policy_ready_.store(false, std::memory_order_release);
    initial_viewport_committed_.store(false, std::memory_order_release);
    active_.store(false, std::memory_order_release);
  }

  bool mark_policy_ready(Session session) {
    if (!is_current(session)) {
      return false;
    }
    policy_ready_.store(true, std::memory_order_release);
    return is_current(session);
  }

  bool commit_initial_viewport(Session session) {
    if (!is_current(session)) {
      return false;
    }
    initial_viewport_committed_.store(true, std::memory_order_release);
    return is_current(session);
  }

  bool accept_present(Session session, bool present_succeeded,
                      bool target_from_current_ring) {
    if (!present_succeeded || !target_from_current_ring ||
        !is_current(session) ||
        !policy_ready_.load(std::memory_order_acquire) ||
        !initial_viewport_committed_.load(std::memory_order_acquire)) {
      return false;
    }
    bool expected = false;
    if (!active_.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel)) {
      return false;
    }
    if (!is_current(session)) {
      active_.store(false, std::memory_order_release);
      return false;
    }
    return true;
  }

  bool active(Session session) const {
    return is_current(session) && active_.load(std::memory_order_acquire);
  }

  bool awaiting_first_frame(Session session) const {
    return is_current(session) && !active_.load(std::memory_order_acquire);
  }

 private:
  bool is_current(Session session) const {
    return session != 0 && session_.load(std::memory_order_acquire) == session;
  }

  std::atomic<Session> session_{0};
  std::atomic<bool> policy_ready_{false};
  std::atomic<bool> initial_viewport_committed_{false};
  std::atomic<bool> active_{false};
};

}  // namespace vr

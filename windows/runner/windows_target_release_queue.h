#pragma once

#include "windows/player/native_player.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

// Releases completed native compositor targets without depending on the Win32
// UI message pump. The renderer callback cannot release synchronously because
// interaction draws still hold the WindowsNativePlayer shared facade lock.
class WindowsTargetReleaseQueue final {
 public:
  WindowsTargetReleaseQueue();
  ~WindowsTargetReleaseQueue();

  WindowsTargetReleaseQueue(const WindowsTargetReleaseQueue&) = delete;
  WindowsTargetReleaseQueue& operator=(const WindowsTargetReleaseQueue&) =
      delete;

  void Enqueue(const std::shared_ptr<vr::WindowsNativePlayer>& player,
               void* target);
  void Drain();

 private:
  struct Request {
    std::shared_ptr<vr::WindowsNativePlayer> player;
    void* target = nullptr;
  };

  void Run();

  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Request> requests_;
  std::thread thread_;
  bool stopping_ = false;
  size_t active_count_ = 0;
  uint64_t enqueue_count_ = 0;
  uint64_t release_count_ = 0;
};

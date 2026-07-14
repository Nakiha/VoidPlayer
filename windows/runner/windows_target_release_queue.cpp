#include "windows_target_release_queue.h"

#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>

WindowsTargetReleaseQueue::WindowsTargetReleaseQueue()
    : thread_(&WindowsTargetReleaseQueue::Run, this) {}

WindowsTargetReleaseQueue::~WindowsTargetReleaseQueue() {
  Drain();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  condition_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void WindowsTargetReleaseQueue::Enqueue(
    const std::shared_ptr<vr::WindowsNativePlayer>& player,
    void* target) {
  if (!player || !target) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    requests_.push_back({player, target});
    const auto count = ++enqueue_count_;
    if (count <= 12 || count % 120 == 0) {
      spdlog::info(
          "[WindowsTargetRelease] enqueue={} released={} queued={} active={}",
          count, release_count_, requests_.size(), active_count_);
    }
  }
  condition_.notify_one();
}

void WindowsTargetReleaseQueue::Drain() {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock,
                  [this]() { return requests_.empty() && active_count_ == 0; });
}

void WindowsTargetReleaseQueue::Run() {
  for (;;) {
    Request request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock,
                      [this]() { return stopping_ || !requests_.empty(); });
      if (stopping_ && requests_.empty()) {
        return;
      }
      request = std::move(requests_.front());
      requests_.pop_front();
      ++active_count_;
    }

    const auto release_started = std::chrono::steady_clock::now();
    request.player->release_target(request.target);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      --active_count_;
      const auto count = ++release_count_;
      if (count <= 12 || count % 120 == 0) {
        spdlog::info(
            "[WindowsTargetRelease] released={} enqueue={} queued={} "
            "elapsed_us={}",
            count, enqueue_count_, requests_.size(),
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - release_started)
                .count());
      }
    }
    condition_.notify_all();
  }
}

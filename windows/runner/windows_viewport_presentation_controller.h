#pragma once

#include <windows.h>

#include "windows/player/native_player.h"

#include <condition_variable>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

struct WindowsViewportPresentationDiagnostics {
  uint64_t layout_intent_count = 0;
  uint64_t layout_submit_count = 0;
  uint64_t layout_presented_count = 0;
  uint64_t layout_failed_count = 0;
  uint64_t layout_backpressure_count = 0;
  uint64_t layout_superseded_count = 0;
  uint64_t latest_intent_serial = 0;
  uint64_t submitted_intent_serial = 0;
  uint32_t interaction_frames_in_flight = 0;
  double nominal_refresh_hz = 0.0;
  double measured_submit_hz = 0.0;
};

// Windows counterpart of MacOSPresentationController's display-linked layout
// lane. Method-channel calls only publish the latest intent; this worker draws
// the cached source frame and lets the compositor's DXGI Present(1) provide the
// display cadence. Source-video presentation remains owned by the renderer.
class WindowsViewportPresentationController final {
 public:
  explicit WindowsViewportPresentationController(HWND window_handle);
  ~WindowsViewportPresentationController();

  WindowsViewportPresentationController(
      const WindowsViewportPresentationController&) = delete;
  WindowsViewportPresentationController& operator=(
      const WindowsViewportPresentationController&) = delete;

  void AttachPlayer(const std::shared_ptr<vr::WindowsNativePlayer>& player);
  void DetachPlayerAndWait();
  void RequestLayoutFrame();
  WindowsViewportPresentationDiagnostics diagnostics() const;

 private:
  void Run();
  double QueryNominalRefreshHz() const;

  HWND window_handle_ = nullptr;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  static constexpr size_t kMaxInteractionFramesInFlight = 2;
  std::array<std::thread, kMaxInteractionFramesInFlight> threads_;
  std::shared_ptr<vr::WindowsNativePlayer> player_;
  bool stopping_ = false;
  WindowsViewportPresentationDiagnostics diagnostics_;
  int64_t last_completion_ns_ = 0;
};

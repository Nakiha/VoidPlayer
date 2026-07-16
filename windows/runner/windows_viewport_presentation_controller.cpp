#include "windows_viewport_presentation_controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include <spdlog/spdlog.h>

#include "renderer/render/renderer_frame_refresh_policy.h"

namespace {

int64_t SteadyClockNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

WindowsViewportPresentationController::WindowsViewportPresentationController(
    HWND window_handle)
    : window_handle_(window_handle) {
  for (auto& thread : threads_) {
    thread = std::thread(&WindowsViewportPresentationController::Run, this);
  }
}

WindowsViewportPresentationController::~WindowsViewportPresentationController() {
  DetachPlayerAndWait();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  condition_.notify_all();
  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

void WindowsViewportPresentationController::AttachPlayer(
    const std::shared_ptr<vr::WindowsNativePlayer>& player) {
  DetachPlayerAndWait();
  std::lock_guard<std::mutex> lock(mutex_);
  player_ = player;
  diagnostics_ = {};
  diagnostics_.nominal_refresh_hz = QueryNominalRefreshHz();
  last_completion_ns_ = 0;
  spdlog::info(
      "[WindowsInteraction] attached clock=dxgi-present-vsync nominal_hz={:.2f} "
      "max_in_flight={}",
      diagnostics_.nominal_refresh_hz, kMaxInteractionFramesInFlight);
}

void WindowsViewportPresentationController::DetachPlayerAndWait() {
  std::unique_lock<std::mutex> lock(mutex_);
  player_.reset();
  diagnostics_.submitted_intent_serial = diagnostics_.latest_intent_serial;
  condition_.notify_all();
  condition_.wait(lock, [this]() {
    return diagnostics_.interaction_frames_in_flight == 0;
  });
}

void WindowsViewportPresentationController::RequestLayoutFrame() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!player_ || stopping_) {
    return;
  }
  if (diagnostics_.latest_intent_serial >
      diagnostics_.submitted_intent_serial) {
    ++diagnostics_.layout_superseded_count;
  }
  ++diagnostics_.layout_intent_count;
  ++diagnostics_.latest_intent_serial;
  const uint64_t count = diagnostics_.layout_intent_count;
  if (count <= 12 || count % 120 == 0) {
    spdlog::info(
        "[WindowsInteraction] intent={} serial={} submitted={} superseded={} "
        "in_flight={}",
        count, diagnostics_.latest_intent_serial,
        diagnostics_.submitted_intent_serial,
        diagnostics_.layout_superseded_count,
        diagnostics_.interaction_frames_in_flight);
  }
  condition_.notify_all();
}

WindowsViewportPresentationDiagnostics
WindowsViewportPresentationController::diagnostics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return diagnostics_;
}

void WindowsViewportPresentationController::Run() {
  for (;;) {
    std::shared_ptr<vr::WindowsNativePlayer> player;
    uint64_t serial = 0;
    uint64_t submit_count = 0;
    double nominal_refresh_hz = 0.0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this]() {
        return stopping_ ||
               (player_ && diagnostics_.latest_intent_serial >
                               diagnostics_.submitted_intent_serial);
      });
      if (stopping_) {
        return;
      }
      player = player_;
      serial = diagnostics_.latest_intent_serial;
      diagnostics_.submitted_intent_serial = serial;
      ++diagnostics_.interaction_frames_in_flight;
      submit_count = ++diagnostics_.layout_submit_count;
      nominal_refresh_hz = diagnostics_.nominal_refresh_hz;
    }

    if (submit_count <= 12 || submit_count % 120 == 0) {
      spdlog::info(
          "[WindowsInteraction] submit={} serial={} clock=dxgi-present-vsync",
          submit_count, serial);
    }
    const auto refresh_result =
        player ? player->request_interaction_frame()
               : vr::RendererFrameRefreshResult::Failed;
    const bool presented =
        refresh_result == vr::RendererFrameRefreshResult::Presented;
    const std::string presentation_error =
        refresh_result == vr::RendererFrameRefreshResult::Failed && player
            ? player->presentation_error()
            : std::string();
    const bool transient_backpressure =
        refresh_result == vr::RendererFrameRefreshResult::Failed &&
        presentation_error == "Windows D3D11 target ring is busy";
    const auto disposition = vr::classify_interaction_refresh_result(
        refresh_result, transient_backpressure);
    const bool transient_not_ready =
        disposition ==
        vr::RendererInteractionRefreshDisposition::RetryNotReady;
    const bool retry =
        transient_not_ready ||
        disposition ==
            vr::RendererInteractionRefreshDisposition::RetryBackpressure;
    if (retry) {
      // Match macOS display-link coalescing: an incomplete multi-track snapshot
      // or a temporarily full target ring is not a failed user interaction.
      // Retry the latest intent at display cadence unless it was superseded.
      const auto retry_ms = static_cast<int>(std::clamp(
          std::lround(1000.0 /
                      (nominal_refresh_hz > 1.0 ? nominal_refresh_hz : 60.0)),
          1L, 17L));
      std::this_thread::sleep_for(std::chrono::milliseconds(retry_ms));
    }
    const int64_t completion_ns = SteadyClockNs();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (presented) {
        ++diagnostics_.layout_presented_count;
        if (last_completion_ns_ > 0 && completion_ns > last_completion_ns_) {
          const double interval_seconds =
              static_cast<double>(completion_ns - last_completion_ns_) / 1e9;
          const double sample_hz = 1.0 / interval_seconds;
          if (sample_hz >= 1.0 && sample_hz <= 1000.0) {
            diagnostics_.measured_submit_hz =
                diagnostics_.measured_submit_hz <= 0.0
                    ? sample_hz
                    : diagnostics_.measured_submit_hz * 0.9 + sample_hz * 0.1;
          }
        }
        last_completion_ns_ = completion_ns;
      } else if (retry) {
        if (transient_not_ready) {
          ++diagnostics_.layout_not_ready_count;
        } else {
          ++diagnostics_.layout_backpressure_count;
        }
        if (player_ == player &&
            diagnostics_.latest_intent_serial == serial &&
            diagnostics_.submitted_intent_serial == serial) {
          diagnostics_.submitted_intent_serial = serial - 1;
        }
      } else {
        ++diagnostics_.layout_failed_count;
      }
      if (diagnostics_.interaction_frames_in_flight > 0) {
        --diagnostics_.interaction_frames_in_flight;
      }
      const uint64_t completed = diagnostics_.layout_presented_count;
      const bool log_failure =
          !presented &&
          (!retry ||
           (transient_not_ready
                ? diagnostics_.layout_not_ready_count <= 8 ||
                      diagnostics_.layout_not_ready_count % 120 == 0
                : diagnostics_.layout_backpressure_count <= 8 ||
                      diagnostics_.layout_backpressure_count % 120 == 0));
      if (completed <= 32 || completed % 120 == 0 || log_failure) {
        spdlog::info(
            "[WindowsInteraction] complete={} serial={} success={} "
            "measured_hz={:.2f} nominal_hz={:.2f} pending={} "
            "not_ready={} backpressure={} error={}",
            completed, serial, presented, diagnostics_.measured_submit_hz,
            diagnostics_.nominal_refresh_hz,
            diagnostics_.latest_intent_serial >
                diagnostics_.submitted_intent_serial,
            diagnostics_.layout_not_ready_count,
            diagnostics_.layout_backpressure_count,
            transient_not_ready ? "frame-not-ready" : presentation_error);
      }
    }
    condition_.notify_all();
  }
}

double WindowsViewportPresentationController::QueryNominalRefreshHz() const {
  const HMONITOR monitor =
      MonitorFromWindow(window_handle_, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW monitor_info = {};
  monitor_info.cbSize = sizeof(monitor_info);
  if (monitor && GetMonitorInfoW(monitor, &monitor_info)) {
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(monitor_info.szDevice, ENUM_CURRENT_SETTINGS,
                             &mode) &&
        mode.dmDisplayFrequency > 1) {
      return static_cast<double>(mode.dmDisplayFrequency);
    }
  }
  return 0.0;
}

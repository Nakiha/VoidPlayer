#include "renderer_event_bridge.h"

#include <algorithm>
#include <chrono>
#include <string>

namespace {

constexpr UINT kDrainMessage = WM_APP + 0x4B7;
constexpr wchar_t kDrainWindowClass[] = L"VoidPlayerRendererEvents";
constexpr size_t kMaxPendingEvents = 256;

LRESULT CALLBACK DrainWindowProc(HWND window,
                                 UINT message,
                                 WPARAM wparam,
                                 LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* bridge = reinterpret_cast<RendererEventBridge*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == kDrainMessage && bridge) {
    bridge->Drain();
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

const char* EventName(vr::RendererEvent::Type type) {
  switch (type) {
    case vr::RendererEvent::Type::SeekPreviewPresented:
      return "seekPreviewPresented";
    case vr::RendererEvent::Type::TrackError:
      return "trackError";
    case vr::RendererEvent::Type::PlaybackClock:
    case vr::RendererEvent::Type::PlaybackFrameReady:
      return "playbackClock";
  }
  return "unknown";
}

flutter::EncodableValue EncodeEvent(const vr::RendererEvent& event,
                                    int64_t sequence) {
  const auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  flutter::EncodableMap payload = {
      {flutter::EncodableValue("schemaVersion"), flutter::EncodableValue(1)},
      {flutter::EncodableValue("sequence"), flutter::EncodableValue(sequence)},
      {flutter::EncodableValue("timestampUs"),
       flutter::EncodableValue(static_cast<int64_t>(timestamp_us))},
      {flutter::EncodableValue("type"),
       flutter::EncodableValue(EventName(event.type))},
      {flutter::EncodableValue("requestId"),
       flutter::EncodableValue(event.request_id)},
      {flutter::EncodableValue("trackFileId"),
       flutter::EncodableValue(event.track_file_id)},
      {flutter::EncodableValue("ptsUs"), flutter::EncodableValue(event.pts_us)},
      {flutter::EncodableValue("dtsUs"), flutter::EncodableValue(event.dts_us)},
      {flutter::EncodableValue("targetPtsUs"),
       flutter::EncodableValue(event.target_pts_us)},
      {flutter::EncodableValue("durationUs"),
       flutter::EncodableValue(event.duration_us)},
      {flutter::EncodableValue("isPlaying"),
       flutter::EncodableValue(event.playing)},
      {flutter::EncodableValue("playbackSpeed"),
       flutter::EncodableValue(event.playback_speed)},
      {flutter::EncodableValue("errorCode"),
       flutter::EncodableValue(event.error_code)},
  };
  return flutter::EncodableValue(std::move(payload));
}

bool IsPlaybackClock(const flutter::EncodableValue& event) {
  const auto* map = std::get_if<flutter::EncodableMap>(&event);
  if (!map) {
    return false;
  }
  const auto found = map->find(flutter::EncodableValue("type"));
  if (found == map->end()) {
    return false;
  }
  const auto* type = std::get_if<std::string>(&found->second);
  return type && *type == "playbackClock";
}

}  // namespace

RendererEventBridge::~RendererEventBridge() {
  Shutdown();
}

void RendererEventBridge::RegisterDrainWindow() {
  if (event_hwnd_) {
    return;
  }
  WNDCLASSW window_class = {};
  window_class.lpfnWndProc = DrainWindowProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = kDrainWindowClass;
  RegisterClassW(&window_class);
  event_hwnd_ = CreateWindowExW(0, kDrainWindowClass, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), this);
}

void RendererEventBridge::Shutdown() {
  if (event_hwnd_) {
    DestroyWindow(event_hwnd_);
    event_hwnd_ = nullptr;
  }
  ClearSink();
}

void RendererEventBridge::SetSink(
    std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
  }
  Drain();
}

void RendererEventBridge::ClearSink() {
  std::lock_guard<std::mutex> lock(mutex_);
  sink_.reset();
  pending_events_.clear();
}

void RendererEventBridge::PostTask(std::function<void()> task) {
  if (!task) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_tasks_.push_back(std::move(task));
  }
  if (event_hwnd_) {
    PostMessageW(event_hwnd_, kDrainMessage, 0, 0);
  }
}

void RendererEventBridge::Queue(const vr::RendererEvent& event) {
  auto payload = EncodeEvent(
      event, sequence_.fetch_add(1, std::memory_order_relaxed) + 1);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event.type == vr::RendererEvent::Type::PlaybackClock ||
        event.type == vr::RendererEvent::Type::PlaybackFrameReady) {
      pending_events_.erase(
          std::remove_if(pending_events_.begin(), pending_events_.end(),
                         IsPlaybackClock),
          pending_events_.end());
    }
    if (pending_events_.size() >= kMaxPendingEvents) {
      pending_events_.pop_front();
    }
    pending_events_.push_back(std::move(payload));
  }
  if (event_hwnd_) {
    PostMessageW(event_hwnd_, kDrainMessage, 0, 0);
  }
}

void RendererEventBridge::QueueNativeCompositorState(
    bool active,
    bool requested,
    const std::string& mode,
    const std::string& phase,
    const std::string& reason,
    const std::string& failure) {
  const auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const int64_t sequence =
      sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  flutter::EncodableMap payload = {
      {flutter::EncodableValue("schemaVersion"), flutter::EncodableValue(1)},
      {flutter::EncodableValue("sequence"), flutter::EncodableValue(sequence)},
      {flutter::EncodableValue("timestampUs"),
       flutter::EncodableValue(static_cast<int64_t>(timestamp_us))},
      {flutter::EncodableValue("type"),
       flutter::EncodableValue("nativeCompositorState")},
      {flutter::EncodableValue("nativeCompositorActive"),
       flutter::EncodableValue(active)},
      {flutter::EncodableValue("nativeCompositorRequested"),
       flutter::EncodableValue(requested)},
      {flutter::EncodableValue("nativeCompositorEDREnabled"),
       flutter::EncodableValue(false)},
      {flutter::EncodableValue("nativeCompositorMode"),
       flutter::EncodableValue(mode)},
      {flutter::EncodableValue("nativeCompositorPhase"),
       flutter::EncodableValue(phase)},
      {flutter::EncodableValue("nativeCompositorSerial"),
       flutter::EncodableValue(int64_t{0})},
      {flutter::EncodableValue("nativeCompositorReason"),
       flutter::EncodableValue(reason)},
      {flutter::EncodableValue("nativeCompositorFailure"),
       flutter::EncodableValue(failure)},
  };
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_events_.size() >= kMaxPendingEvents) {
      pending_events_.pop_front();
    }
    pending_events_.emplace_back(std::move(payload));
  }
  if (event_hwnd_) {
    PostMessageW(event_hwnd_, kDrainMessage, 0, 0);
  }
}

void RendererEventBridge::Drain() {
  for (;;) {
    std::function<void()> task;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!pending_tasks_.empty()) {
        task = std::move(pending_tasks_.front());
        pending_tasks_.pop_front();
      } else if (sink_ && !pending_events_.empty()) {
        auto event = std::move(pending_events_.front());
        pending_events_.pop_front();
        sink_->Success(event);
        continue;
      } else {
        return;
      }
    }
    // Renderer release calls may synchronously trigger more work and must run
    // outside the bridge mutex.
    task();
  }
}

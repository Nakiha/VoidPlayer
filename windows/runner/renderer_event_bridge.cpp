#include "renderer_event_bridge.h"

#include <flutter/encodable_value.h>
#include <spdlog/spdlog.h>

#include <chrono>

namespace {

constexpr UINT kVideoRendererEventDrainMessage = WM_APP + 0x4B7;
constexpr wchar_t kVideoRendererEventWindowClass[] = L"VoidPlayerVideoRendererEvents";
constexpr size_t kMaxPendingRendererEvents = 256;

LRESULT CALLBACK RendererEventBridgeWindowProc(HWND hwnd,
                                               UINT message,
                                               WPARAM wparam,
                                               LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    auto* bridge = reinterpret_cast<RendererEventBridge*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == kVideoRendererEventDrainMessage && bridge) {
        bridge->Drain();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

flutter::EncodableValue make_event_payload(const vr::RendererEvent& event,
                                           int64_t sequence) {
    flutter::EncodableMap payload;
    payload[flutter::EncodableValue("schemaVersion")] = flutter::EncodableValue(1);
    payload[flutter::EncodableValue("sequence")] = flutter::EncodableValue(sequence);
    payload[flutter::EncodableValue("timestampUs")] =
        flutter::EncodableValue(static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()));
    switch (event.type) {
    case vr::RendererEvent::Type::SeekPreviewPresented:
        payload[flutter::EncodableValue("type")] =
            flutter::EncodableValue("seekPreviewPresented");
        break;
    case vr::RendererEvent::Type::TrackError:
        payload[flutter::EncodableValue("type")] =
            flutter::EncodableValue("trackError");
        break;
    }
    payload[flutter::EncodableValue("requestId")] = flutter::EncodableValue(event.request_id);
    payload[flutter::EncodableValue("trackFileId")] =
        flutter::EncodableValue(event.track_file_id);
    payload[flutter::EncodableValue("ptsUs")] = flutter::EncodableValue(event.pts_us);
    payload[flutter::EncodableValue("dtsUs")] = flutter::EncodableValue(event.dts_us);
    payload[flutter::EncodableValue("targetPtsUs")] =
        flutter::EncodableValue(event.target_pts_us);
    payload[flutter::EncodableValue("errorCode")] = flutter::EncodableValue(event.error_code);
    return flutter::EncodableValue(std::move(payload));
}

} // namespace

RendererEventBridge::~RendererEventBridge() {
    Shutdown();
}

void RendererEventBridge::RegisterDrainWindow() {
    if (event_hwnd_) {
        return;
    }
    WNDCLASSW wc = {};
    wc.lpfnWndProc = RendererEventBridgeWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kVideoRendererEventWindowClass;
    RegisterClassW(&wc);
    event_hwnd_ = CreateWindowExW(
        0,
        kVideoRendererEventWindowClass,
        L"",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        GetModuleHandleW(nullptr),
        this);
    if (!event_hwnd_) {
        spdlog::warn("[RendererEventBridge] failed to create renderer event message window");
    }
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
        ++listen_count_;
    }
    Drain();
}

void RendererEventBridge::ClearSink() {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_.reset();
    pending_events_.clear();
}

void RendererEventBridge::Queue(const vr::RendererEvent& event) {
    const int64_t sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    auto payload = make_event_payload(event, sequence);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sink_) {
            ++drop_no_sink_count_;
        }
        if (pending_events_.size() >= kMaxPendingRendererEvents) {
            pending_events_.pop_front();
            spdlog::warn("[RendererEventBridge] renderer event queue overflow, "
                         "dropped oldest event");
        }
        pending_events_.emplace_back(std::move(payload));
    }
    spdlog::debug("[RendererEventBridge] queued renderer event request_id={} file_id={}",
                  event.request_id, event.track_file_id);
    if (event_hwnd_) {
        PostMessage(event_hwnd_, kVideoRendererEventDrainMessage, 0, 0);
    }
}

void RendererEventBridge::Drain() {
    for (;;) {
        flutter::EncodableValue event;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!sink_ || pending_events_.empty()) {
                return;
            }
            event = std::move(pending_events_.front());
            pending_events_.pop_front();
            spdlog::debug("[RendererEventBridge] draining renderer event");
            sink_->Success(event);
            ++emit_count_;
        }
    }
}

RendererEventBridge::Diagnostics RendererEventBridge::diagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        listen_count_,
        emit_count_,
        drop_no_sink_count_,
    };
}

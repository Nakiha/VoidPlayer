#pragma once

#include <string>

#include <flutter/standard_method_codec.h>
#include <flutter/event_sink.h>
#include <windows.h>

#include "windows/player/native_player.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

class RendererEventBridge {
public:
    struct Diagnostics {
        int64_t listen_count = 0;
        int64_t emit_count = 0;
        int64_t drop_no_sink_count = 0;
    };

    RendererEventBridge() = default;
    ~RendererEventBridge();

    RendererEventBridge(const RendererEventBridge&) = delete;
    RendererEventBridge& operator=(const RendererEventBridge&) = delete;

    void RegisterDrainWindow();
    void Shutdown();

    void SetSink(std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink);
    void ClearSink();
    void Queue(const vr::RendererEvent& event);
    void QueueNativeCompositorState(bool active,
                                    bool requested,
                                    bool edr_enabled,
                                    const std::string& mode,
                                    const std::string& phase,
                                    int64_t serial,
                                    const std::string& reason,
                                    const std::string& failure);
    void Drain();
    Diagnostics diagnostics() const;

private:
    HWND event_hwnd_ = nullptr;
    mutable std::mutex mutex_;
    std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink_;
    std::deque<flutter::EncodableValue> pending_events_;
    std::atomic<int64_t> sequence_{0};
    int64_t listen_count_ = 0;
    int64_t emit_count_ = 0;
    int64_t drop_no_sink_count_ = 0;
};

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
    RendererEventBridge() = default;
    ~RendererEventBridge();

    RendererEventBridge(const RendererEventBridge&) = delete;
    RendererEventBridge& operator=(const RendererEventBridge&) = delete;

    void RegisterDrainWindow();
    void Shutdown();

    void SetSink(std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink);
    void ClearSink();
    void Queue(const vr::RendererEvent& event);
    void Drain();

private:
    HWND event_hwnd_ = nullptr;
    std::mutex mutex_;
    std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink_;
    std::deque<flutter::EncodableValue> pending_events_;
    std::atomic<int64_t> sequence_{0};
};

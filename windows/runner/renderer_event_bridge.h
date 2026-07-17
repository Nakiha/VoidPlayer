#pragma once

#include "renderer/renderer_api_types.h"

#include <flutter/event_sink.h>
#include <flutter/standard_method_codec.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

class RendererEventBridge final {
 public:
  RendererEventBridge() = default;
  ~RendererEventBridge();

  RendererEventBridge(const RendererEventBridge&) = delete;
  RendererEventBridge& operator=(const RendererEventBridge&) = delete;

  void RegisterDrainWindow();
  void Shutdown();
  void SetSink(
      std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink);
  void ClearSink();
  void Queue(const vr::RendererEvent& event);
  void QueueNativeCompositorState(bool active,
                                  bool requested,
                                  const std::string& mode,
                                  const std::string& phase,
                                  const std::string& reason,
                                  const std::string& failure);
  void PostTask(std::function<void()> task);
  void Drain();

 private:
  HWND event_hwnd_ = nullptr;
  mutable std::mutex mutex_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink_;
  std::deque<flutter::EncodableValue> pending_events_;
  std::deque<std::function<void()>> pending_tasks_;
  std::atomic<int64_t> sequence_{0};
};

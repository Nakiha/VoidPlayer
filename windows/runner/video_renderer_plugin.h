#pragma once

#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter_windows.h>

#include "file_picker_service.h"
#include "renderer_event_bridge.h"
#include "windows/player/native_player.h"
#include "windows_native_compositor.h"

#include <atomic>
#include <memory>

class VideoRendererPlugin final : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(
      flutter::PluginRegistrarWindows* registrar,
      FlutterDesktopPluginRegistrarRef core_registrar);

  VideoRendererPlugin(HWND window_handle, FlutterDesktopViewRef flutter_view);
  ~VideoRendererPlugin() override;

  VideoRendererPlugin(const VideoRendererPlugin&) = delete;
  VideoRendererPlugin& operator=(const VideoRendererPlugin&) = delete;

 private:
  using MethodResult = flutter::MethodResult<flutter::EncodableValue>;

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& call,
      std::unique_ptr<MethodResult> result);
  void SetEventSink(
      std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink);
  void ClearEventSink();
  void DestroyPlayer();
  bool ResizeVideoTargets(int width, int height, std::string& error);
  void OnFrameAvailable(
      const std::weak_ptr<vr::WindowsNativePlayer>& weak_player,
      const vr::PresentationBackendFrameInfo* frame_info);

  FilePickerService file_picker_;
  RendererEventBridge event_bridge_;
  HWND window_handle_ = nullptr;
  std::unique_ptr<WindowsNativeCompositor> compositor_;
  bool compositor_started_ = false;
  std::shared_ptr<vr::WindowsNativePlayer> player_;
  int video_target_width_ = 0;
  int video_target_height_ = 0;
  int64_t player_id_ = 0;
  std::atomic<int64_t> next_player_id_{1};
};

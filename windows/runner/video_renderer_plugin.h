#pragma once

#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter_windows.h>

#include "file_picker_service.h"
#include "windows_native_compositor.h"

#include <memory>

class VideoRendererPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(
      flutter::PluginRegistrarWindows* registrar,
      FlutterDesktopPluginRegistrarRef core_registrar);

  VideoRendererPlugin(HWND window_handle, FlutterDesktopViewRef flutter_view);
  ~VideoRendererPlugin() override;

  VideoRendererPlugin(const VideoRendererPlugin&) = delete;
  VideoRendererPlugin& operator=(const VideoRendererPlugin&) = delete;

 private:
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  FilePickerService file_picker_;
  HWND window_handle_ = nullptr;
  std::unique_ptr<WindowsNativeCompositor> compositor_;
  bool compositor_started_ = false;
};

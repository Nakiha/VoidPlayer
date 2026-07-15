#pragma once

#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter_windows.h>

#include "file_picker_service.h"
#include "renderer_event_bridge.h"
#include "windows/player/native_player.h"
#include "windows/presentation/windows_first_frame_activation_gate.h"
#include "windows/presentation/windows_display_resolver.h"
#include "windows/presentation/windows_presentation_policy.h"
#include "windows_native_compositor.h"
#include "windows_target_release_queue.h"
#include "windows_viewport_presentation_controller.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

class VideoRendererPlugin final : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(
      flutter::PluginRegistrarWindows* registrar,
      FlutterDesktopPluginRegistrarRef core_registrar);

  VideoRendererPlugin(flutter::PluginRegistrarWindows* registrar,
                      HWND window_handle, FlutterDesktopViewRef flutter_view);
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
  std::optional<LRESULT> HandleTopLevelWindowProc(HWND hwnd, UINT message,
                                                  WPARAM wparam, LPARAM lparam);
  void SchedulePresentationPolicyRefresh();
  void FailClosedPresentation(std::string failure);
  bool RefreshPresentationPolicy(const char* reason, std::string& error);
  bool ApplyPresentationPolicy(vr::WindowsPresentationPolicy policy,
                               const vr::WindowsDisplayProbeResult& display,
                               const char* reason,
                               std::string& error);
  bool ResizeVideoTargets(int width, int height, std::string& error);
  void QueueCurrentNativeCompositorState(
      vr::WindowsFirstFrameActivationGate::Session expected_session = 0);
  void OnFrameAvailable(
      const std::weak_ptr<vr::WindowsNativePlayer>& weak_player,
      vr::WindowsFirstFrameActivationGate::Session presentation_session,
      const vr::PresentationBackendFrameInfo* frame_info);

  FilePickerService file_picker_;
  RendererEventBridge event_bridge_;
  WindowsTargetReleaseQueue target_release_queue_;
  HWND window_handle_ = nullptr;
  flutter::PluginRegistrarWindows* registrar_ = nullptr;
  int window_proc_delegate_id_ = -1;
  std::unique_ptr<WindowsNativeCompositor> compositor_;
  std::unique_ptr<WindowsViewportPresentationController>
      viewport_presentation_controller_;
  bool compositor_started_ = false;
  std::shared_ptr<vr::WindowsNativePlayer> player_;
  int video_target_width_ = 0;
  int video_target_height_ = 0;
  uint64_t layout_apply_count_ = 0;
  vr::WindowsDisplayResolver display_resolver_;
  vr::WindowsDisplayProbeResult display_probe_;
  vr::WindowsPresentationPolicy presentation_policy_;
  double presentation_sdr_white_level_nits_ = 80.0;
  mutable std::mutex presentation_state_mutex_;
  vr::WindowsFirstFrameActivationGate first_frame_activation_gate_;
  vr::WindowsFirstFrameActivationGate::Session presentation_session_ = 0;
  int64_t player_id_ = 0;
  std::atomic<int64_t> next_player_id_{1};
};

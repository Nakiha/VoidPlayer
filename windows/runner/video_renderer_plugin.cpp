#include "video_renderer_plugin.h"

#include "native_player_channel_names.h"

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>

#include <string>
#include <variant>

namespace {

constexpr char kUnavailableReason[] =
    "Windows native presentation backend has not been rebuilt";

bool ReadBool(const flutter::EncodableValue* arguments,
              const char* key,
              bool fallback) {
  const auto* map = arguments
      ? std::get_if<flutter::EncodableMap>(arguments)
      : nullptr;
  if (!map) {
    return fallback;
  }
  const auto it = map->find(flutter::EncodableValue(key));
  if (it == map->end()) {
    return fallback;
  }
  const auto* value = std::get_if<bool>(&it->second);
  return value ? *value : fallback;
}

flutter::EncodableMap UnavailableDiagnostics(
    const WindowsNativeCompositor* compositor,
    bool compositor_started) {
  flutter::EncodableMap diagnostics = {
      {flutter::EncodableValue("available"), flutter::EncodableValue(false)},
      {flutter::EncodableValue("reason"),
       flutter::EncodableValue(kUnavailableReason)},
      {flutter::EncodableValue("presentationBackend"),
       flutter::EncodableValue("windows-native-unavailable")},
      {flutter::EncodableValue("nativeCompositorEnabled"),
       flutter::EncodableValue(compositor_started)},
      {flutter::EncodableValue("windowsBackendRebuildRequired"),
       flutter::EncodableValue(true)},
  };
  if (compositor) {
    const auto state = compositor->diagnostics();
    diagnostics[flutter::EncodableValue("windowsCompositorInitialized")] =
        flutter::EncodableValue(state.initialized);
    diagnostics[flutter::EncodableValue("windowsFlutterExportEnabled")] =
        flutter::EncodableValue(state.flutter_export_enabled);
    diagnostics[flutter::EncodableValue("windowsCompositeCount")] =
        flutter::EncodableValue(static_cast<int64_t>(state.composite_count));
    diagnostics[flutter::EncodableValue("windowsFlutterPublishCount")] =
        flutter::EncodableValue(
            static_cast<int64_t>(state.flutter_publish_count));
    diagnostics[flutter::EncodableValue("windowsAcquireFailureCount")] =
        flutter::EncodableValue(
            static_cast<int64_t>(state.acquire_failure_count));
    diagnostics[flutter::EncodableValue("windowsKeyedMutexFailureCount")] =
        flutter::EncodableValue(
            static_cast<int64_t>(state.keyed_mutex_failure_count));
    diagnostics[flutter::EncodableValue("windowsPresentFailureCount")] =
        flutter::EncodableValue(
            static_cast<int64_t>(state.present_failure_count));
    diagnostics[flutter::EncodableValue("windowsLastFlutterFrameGeneration")] =
        flutter::EncodableValue(
            static_cast<int64_t>(state.last_flutter_frame_generation));
    diagnostics[flutter::EncodableValue("windowsCompositorLastError")] =
        flutter::EncodableValue(state.last_error);
  }
  return diagnostics;
}

}  // namespace

void VideoRendererPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar,
    FlutterDesktopPluginRegistrarRef core_registrar) {
  (void)core_registrar;
  auto channel = std::make_unique<
      flutter::MethodChannel<flutter::EncodableValue>>(
      registrar->messenger(),
      native_player_channels::kMethodChannel,
      &flutter::StandardMethodCodec::GetInstance());
  auto events = std::make_unique<
      flutter::EventChannel<flutter::EncodableValue>>(
      registrar->messenger(),
      native_player_channels::kEventChannel,
      &flutter::StandardMethodCodec::GetInstance());

  FlutterDesktopViewRef flutter_view =
      FlutterDesktopPluginRegistrarGetView(core_registrar);
  HWND flutter_window = flutter_view
      ? FlutterDesktopViewGetHWND(flutter_view)
      : nullptr;
  HWND window_handle = flutter_window
      ? GetAncestor(flutter_window, GA_ROOT)
      : nullptr;
  auto plugin =
      std::make_unique<VideoRendererPlugin>(window_handle, flutter_view);
  channel->SetMethodCallHandler(
      [plugin_ptr = plugin.get()](const auto& call, auto result) {
        plugin_ptr->HandleMethodCall(call, std::move(result));
      });
  events->SetStreamHandler(std::make_unique<
      flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
      [](const flutter::EncodableValue*,
         std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&&)
          -> std::unique_ptr<
              flutter::StreamHandlerError<flutter::EncodableValue>> {
        return nullptr;
      },
      [](const flutter::EncodableValue*)
          -> std::unique_ptr<
              flutter::StreamHandlerError<flutter::EncodableValue>> {
        return nullptr;
      }));
  registrar->AddPlugin(std::move(plugin));
}

VideoRendererPlugin::VideoRendererPlugin(HWND window_handle,
                                         FlutterDesktopViewRef flutter_view)
    : window_handle_(window_handle) {
  if (window_handle_ && flutter_view) {
    compositor_ = std::make_unique<WindowsNativeCompositor>(
        window_handle_, flutter_view);
    compositor_started_ = compositor_->Start();
  }
}

VideoRendererPlugin::~VideoRendererPlugin() {
  if (compositor_) {
    compositor_->Stop();
  }
}

void VideoRendererPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const std::string& method = call.method_name();
  if (method == "initLogging" || method == "destroyPlayer" ||
      method == "resetNativePerfCounters" ||
      method == "clearNativeCompositorSourceCache") {
    result->Success();
    return;
  }
  if (method == "pickFiles") {
    const bool allow_multiple =
        ReadBool(call.arguments(), "allowMultiple", true);
    flutter::EncodableList files;
    for (const auto& path :
         file_picker_.PickVideoFiles(allow_multiple, window_handle_)) {
      files.emplace_back(path);
    }
    result->Success(flutter::EncodableValue(std::move(files)));
    return;
  }
  if (method == "getDiagnostics" || method == "debugNativeCompositor") {
    result->Success(flutter::EncodableValue(
        UnavailableDiagnostics(compositor_.get(), compositor_started_)));
    return;
  }
  if (method == "getTracks") {
    result->Success(flutter::EncodableValue(flutter::EncodableList{}));
    return;
  }
  if (method == "createPlayer" || method == "addTrack") {
    result->Error("BACKEND_UNAVAILABLE", kUnavailableReason,
                  flutter::EncodableValue(UnavailableDiagnostics(
                      compositor_.get(), compositor_started_)));
    return;
  }
  result->NotImplemented();
}

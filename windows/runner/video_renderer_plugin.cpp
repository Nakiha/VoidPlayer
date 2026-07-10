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

flutter::EncodableMap UnavailableDiagnostics() {
  return {
      {flutter::EncodableValue("available"), flutter::EncodableValue(false)},
      {flutter::EncodableValue("reason"),
       flutter::EncodableValue(kUnavailableReason)},
      {flutter::EncodableValue("presentationBackend"),
       flutter::EncodableValue("windows-native-unavailable")},
      {flutter::EncodableValue("nativeCompositorEnabled"),
       flutter::EncodableValue(false)},
      {flutter::EncodableValue("windowsBackendRebuildRequired"),
       flutter::EncodableValue(true)},
  };
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

  HWND window_handle = nullptr;
  if (auto* view = registrar->GetView()) {
    window_handle = view->GetNativeWindow();
  }
  auto plugin = std::make_unique<VideoRendererPlugin>(window_handle);
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

VideoRendererPlugin::VideoRendererPlugin(HWND window_handle)
    : window_handle_(window_handle) {}

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
    result->Success(flutter::EncodableValue(UnavailableDiagnostics()));
    return;
  }
  if (method == "getTracks") {
    result->Success(flutter::EncodableValue(flutter::EncodableList{}));
    return;
  }
  if (method == "createPlayer" || method == "addTrack") {
    result->Error("BACKEND_UNAVAILABLE", kUnavailableReason,
                  flutter::EncodableValue(UnavailableDiagnostics()));
    return;
  }
  result->NotImplemented();
}

#include "flutter_window.h"

#include <optional>
#include <variant>

#include "flutter/generated_plugin_registrant.h"
#include "startup_trace.h"
#include "video_renderer_plugin.h"

namespace {

constexpr const char kSystemAccentColorChangedMethod[] =
    "systemAccentColorChanged";
constexpr const char kShortcutChannelName[] =
    "void_player/windows_runner_shortcuts";

bool IsApplicationShortcutKey(WPARAM virtual_key) {
  switch (virtual_key) {
    case VK_SPACE:
    case VK_LEFT:
    case VK_RIGHT:
    case 'M':
    case 'O':
    case VK_F11:
    case VK_ESCAPE:
      return true;
    default:
      return false;
  }
}

bool ReadBoolArgument(const flutter::EncodableValue* arguments,
                      const char* key,
                      bool fallback) {
  if (!arguments) {
    return fallback;
  }
  const auto* map = std::get_if<flutter::EncodableMap>(arguments);
  if (!map) {
    return fallback;
  }
  auto it = map->find(flutter::EncodableValue(key));
  if (it == map->end()) {
    return fallback;
  }
  const auto* value = std::get_if<bool>(&it->second);
  return value ? *value : fallback;
}

}  // namespace

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  RunnerStartupTraceMark("FlutterWindow OnCreate entered");
  if (!Win32Window::OnCreate()) {
    return false;
  }
  RunnerStartupTraceMark("base OnCreate completed");

  RECT frame = GetClientArea();
  RunnerStartupTraceMark("client area resolved");

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  RunnerStartupTraceMark("FlutterViewController created");
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  RunnerStartupTraceMark("generated plugins registered");

  // Register video renderer plugin (built into runner, not a pub package)
  FlutterDesktopPluginRegistrarRef video_renderer_registrar =
      flutter_controller_->engine()->GetRegistrarForPlugin(
          "VideoRendererPlugin");
  VideoRendererPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(
              video_renderer_registrar),
      video_renderer_registrar);
  RunnerStartupTraceMark("video renderer plugin registered");

  flutter_view_hwnd_ = flutter_controller_->view()->GetNativeWindow();
  SetChildContent(flutter_view_hwnd_);
  RunnerStartupTraceMark("Flutter child content attached");

  bootstrap_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(),
          "void_player/window_bootstrap",
          &flutter::StandardMethodCodec::GetInstance());
  bootstrap_channel_->SetMethodCallHandler(
      [this](const auto& call, auto result) {
        if (call.method_name() != "showAfterNextFrame") {
          result->NotImplemented();
          return;
        }

        const bool inactive =
            ReadBoolArgument(call.arguments(), "inactive", false);
        flutter_controller_->engine()->SetNextFrameCallback([this, inactive]() {
          HWND window = GetHandle();
          if (!window) {
            return;
          }
          ShowWindow(window, inactive ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
        });
        flutter_controller_->ForceRedraw();
        result->Success();
      });
  RunnerStartupTraceMark("bootstrap channel installed");

  shortcut_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(), kShortcutChannelName,
          &flutter::StandardMethodCodec::GetInstance());
  RunnerStartupTraceMark("shortcut channel installed");

  return true;
}

void FlutterWindow::OnDestroy() {
  shortcut_channel_.reset();
  bootstrap_channel_.reset();
  flutter_view_hwnd_ = nullptr;
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

void FlutterWindow::ForwardShortcutMessage(const MSG& message) {
  if (!shortcut_channel_ ||
      (message.hwnd != flutter_view_hwnd_ && message.hwnd != GetHandle()) ||
      (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) ||
      !IsApplicationShortcutKey(message.wParam)) {
    return;
  }

  const auto bits = static_cast<uint64_t>(message.lParam);
  flutter::EncodableMap arguments;
  arguments[flutter::EncodableValue("virtualKey")] =
      flutter::EncodableValue(static_cast<int64_t>(message.wParam));
  arguments[flutter::EncodableValue("scanCode")] =
      flutter::EncodableValue(static_cast<int64_t>((bits >> 16u) & 0xffu));
  arguments[flutter::EncodableValue("repeat")] =
      flutter::EncodableValue(((bits >> 30u) & 1u) != 0);
  arguments[flutter::EncodableValue("controlPressed")] =
      flutter::EncodableValue((::GetKeyState(VK_CONTROL) & 0x8000) != 0);
  shortcut_channel_->InvokeMethod(
      "keyDown",
      std::make_unique<flutter::EncodableValue>(std::move(arguments)));
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
      if (bootstrap_channel_) {
        bootstrap_channel_->InvokeMethod(kSystemAccentColorChangedMethod,
                                         nullptr);
      }
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}

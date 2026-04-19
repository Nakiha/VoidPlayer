#include "flutter_window.h"

#include <optional>
#include <commctrl.h>
#include <spdlog/spdlog.h>

#include "utils.h"

#pragma comment(lib, "comctl32.lib")

#include "flutter/generated_plugin_registrant.h"
#include "video_renderer_plugin.h"
#include "desktop_multi_window/desktop_multi_window_plugin.h"
#include <desktop_drop/desktop_drop_plugin.h>
#include <flutter_acrylic/flutter_acrylic_plugin.h>
#include <screen_retriever_windows/screen_retriever_windows_plugin_c_api.h>

// Minimum size applied to secondary windows (matches main window).
static constexpr int kSecondaryMinWidth = 520;
static constexpr int kSecondaryMinHeight = 360;

static LRESULT CALLBACK SecondaryWindowSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*uidSubclass*/, DWORD_PTR /*dwRefData*/) {
  if (msg == WM_GETMINMAXINFO) {
    auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
    mmi->ptMinTrackSize.x = kSecondaryMinWidth;
    mmi->ptMinTrackSize.y = kSecondaryMinHeight;
    return 0;
  }
  if (msg == WM_CLOSE) {
    if (wParam == 1) {
      // Force close (app shutdown) — allow default processing to destroy window
      spdlog::info("[SecondaryWindow] force close: hwnd={:#x}", (uintptr_t)hwnd);
      return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
    // User clicked X — hide instead of destroy to keep engine alive
    spdlog::info("[SecondaryWindow] user close → hide: hwnd={:#x}", (uintptr_t)hwnd);
    ShowWindow(hwnd, SW_HIDE);
    return 0;
  }
  if (msg == WM_NCDESTROY) {
    RemoveWindowSubclass(hwnd, SecondaryWindowSubclassProc, 0);
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());

  // Register video renderer plugin (built into runner, not a pub package)
  VideoRendererPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(
              flutter_controller_->engine()->GetRegistrarForPlugin(
                  "VideoRendererPlugin")));

  // Register plugins for secondary windows created by desktop_multi_window.
  //
  // NOTE: window_manager is intentionally excluded.  Its native plugin stores a
  // global static method-channel pointer that is overwritten each time the
  // plugin is constructed.  If a secondary window triggers that constructor the
  // channel is repointed to the secondary engine, so the main window's
  // WM_CLOSE event is emitted on the wrong channel and the Dart-side
  // onWindowClose listener never fires.  Combined with setPreventClose(true)
  // on the main window, WM_CLOSE is swallowed entirely and the close button
  // becomes unresponsive.
  DesktopMultiWindowSetWindowCreatedCallback([](void *controller) {
    static int create_count = 0;
    LogStackUsage(fmt::format("secondary_window_create #{}", ++create_count).c_str());

    auto *flutter_view_controller =
        reinterpret_cast<flutter::FlutterViewController *>(controller);
    auto *registry = flutter_view_controller->engine();
    DesktopDropPluginRegisterWithRegistrar(
        registry->GetRegistrarForPlugin("DesktopDropPlugin"));
    FlutterAcrylicPluginRegisterWithRegistrar(
        registry->GetRegistrarForPlugin("FlutterAcrylicPlugin"));
    ScreenRetrieverWindowsPluginCApiRegisterWithRegistrar(
        registry->GetRegistrarForPlugin(
            "ScreenRetrieverWindowsPluginCApi"));
    // VideoRendererPlugin is intentionally NOT registered for secondary
    // windows.  Secondary windows only use dart:ffi (AnalysisFfi) — they
    // don't need the method channel or texture bridge.  Registering the
    // plugin crashes when the main window's render loop is actively
    // updating textures (Dart_InvokeClosure on invalid closure).

    // Apply minimum size constraint to the secondary top-level window.
    auto *view = flutter_view_controller->view();
    if (view) {
      HWND top_level = GetAncestor(view->GetNativeWindow(), GA_ROOT);
      if (top_level) {
        SetWindowSubclass(top_level, SecondaryWindowSubclassProc, 0, 0);
      }
    }

    // Show window after first frame renders to prevent white flash.
    flutter_view_controller->engine()->SetNextFrameCallback(
        [flutter_view_controller]() {
          auto *v = flutter_view_controller->view();
          if (v) {
            HWND top_level = GetAncestor(v->GetNativeWindow(), GA_ROOT);
            if (top_level) {
              ShowWindow(top_level, SW_SHOWNORMAL);
            }
          }
        });
    flutter_view_controller->ForceRedraw();
  });

  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
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
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}

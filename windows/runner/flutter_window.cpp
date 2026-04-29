#include "flutter_window.h"

#include <cstdint>
#include <dwmapi.h>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

#include "flutter/generated_plugin_registrant.h"
#include "video_renderer_plugin.h"

#include <spdlog/spdlog.h>

namespace {

constexpr wchar_t kDCompProbeWindowClass[] = L"VOID_PLAYER_DCOMP_PROBE";

LRESULT CALLBACK DCompProbeWndProc(HWND hwnd,
                                   UINT message,
                                   WPARAM wparam,
                                   LPARAM lparam) {
  return DefWindowProc(hwnd, message, wparam, lparam);
}

void RegisterDCompProbeWindowClass() {
  static bool registered = false;
  if (registered) {
    return;
  }

  WNDCLASS window_class{};
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.lpszClassName = kDCompProbeWindowClass;
  window_class.hInstance = GetModuleHandle(nullptr);
  window_class.hbrBackground =
      reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  window_class.lpfnWndProc = DCompProbeWndProc;
  RegisterClass(&window_class);
  registered = true;
}

std::string HrString(HRESULT hr) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase << std::setw(8)
         << std::setfill('0') << static_cast<unsigned long>(hr);
  return stream.str();
}

std::string PtrString(const void* pointer) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase
         << reinterpret_cast<uintptr_t>(pointer);
  return stream.str();
}

void WindowProbeLog(const std::string& message) {
  const std::string line = "[DCompHDR][Window] " + message;
  spdlog::info("{}", line);
  OutputDebugStringA((line + "\n").c_str());

  std::ofstream file("dcomp_hdr_probe.log", std::ios::app);
  if (file.is_open()) {
    file << line << "\n";
  }
}

}  // namespace

FlutterWindow::FlutterWindow(const flutter::DartProject& project,
                             bool dcomp_alpha_probe)
    : project_(project), dcomp_alpha_probe_enabled_(dcomp_alpha_probe) {}

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

  HWND flutter_hwnd = flutter_controller_->view()->GetNativeWindow();
  HRESULT host_dwm_hr = S_FALSE;
  HRESULT flutter_dwm_hr = S_FALSE;
  BOOL host_color_key_ok = FALSE;
  BOOL flutter_color_key_ok = FALSE;
  BOOL dcomp_child_created = FALSE;
  BOOL dcomp_child_positioned = FALSE;
  BOOL flutter_child_positioned = FALSE;
  if (dcomp_alpha_probe_enabled_) {
    MARGINS margins = {-1, -1, -1, -1};
    host_dwm_hr = DwmExtendFrameIntoClientArea(GetHandle(), &margins);
    flutter_dwm_hr = DwmExtendFrameIntoClientArea(flutter_hwnd, &margins);
    LONG_PTR host_ex_style = GetWindowLongPtr(GetHandle(), GWL_EXSTYLE);
    SetWindowLongPtr(GetHandle(), GWL_EXSTYLE, host_ex_style | WS_EX_LAYERED);
    host_color_key_ok =
        SetLayeredWindowAttributes(GetHandle(), RGB(0, 255, 255), 255,
                                   LWA_COLORKEY);
    RegisterDCompProbeWindowClass();
    RECT rect = GetClientArea();
    dcomp_probe_hwnd_ = CreateWindowEx(
        0, kDCompProbeWindowClass, L"", WS_CHILD | WS_VISIBLE, rect.left,
        rect.top, rect.right - rect.left, rect.bottom - rect.top, GetHandle(),
        nullptr, GetModuleHandle(nullptr), nullptr);
    dcomp_child_created = dcomp_probe_hwnd_ != nullptr;
  }

  SetChildContent(flutter_hwnd);

  if (dcomp_alpha_probe_enabled_) {
    LONG_PTR ex_style = GetWindowLongPtr(flutter_hwnd, GWL_EXSTYLE);
    SetWindowLongPtr(flutter_hwnd, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
    flutter_color_key_ok =
        SetLayeredWindowAttributes(flutter_hwnd, RGB(0, 255, 255), 255,
                                   LWA_COLORKEY);
    SetWindowPos(GetHandle(), nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    SetWindowPos(flutter_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    if (dcomp_probe_hwnd_) {
      RECT rect = GetClientArea();
      dcomp_child_positioned =
          SetWindowPos(dcomp_probe_hwnd_, HWND_BOTTOM, rect.left, rect.top,
                       rect.right - rect.left, rect.bottom - rect.top,
                       SWP_NOACTIVATE);
      flutter_child_positioned =
          SetWindowPos(flutter_hwnd, HWND_TOP, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
  }

  bool native_probe_initialized = false;
  if (dcomp_alpha_probe_enabled_) {
    dcomp_alpha_probe_ = std::make_unique<DCompAlphaProbe>();
    native_probe_initialized =
        dcomp_alpha_probe_->Initialize(dcomp_probe_hwnd_ ? dcomp_probe_hwnd_
                                                         : flutter_hwnd,
                                       GetHandle(), true);
    WindowProbeLog("host=" + PtrString(GetHandle()) +
                   " flutter=" + PtrString(flutter_hwnd) +
                   " dcomp_child=" + PtrString(dcomp_probe_hwnd_) +
                   " hostDwm=" + HrString(host_dwm_hr) +
                   " flutterDwm=" + HrString(flutter_dwm_hr) +
                   " hostColorKey=" +
                   std::to_string(host_color_key_ok ? 1 : 0) +
                   " flutterColorKey=" +
                   std::to_string(flutter_color_key_ok ? 1 : 0) +
                   " dcompChildCreated=" +
                   std::to_string(dcomp_child_created ? 1 : 0) +
                   " dcompChildPositioned=" +
                   std::to_string(dcomp_child_positioned ? 1 : 0) +
                   " flutterChildPositioned=" +
                   std::to_string(flutter_child_positioned ? 1 : 0) +
                   " nativeProbeInitialized=" +
                   std::to_string(native_probe_initialized ? 1 : 0));
  }

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
  if (dcomp_alpha_probe_) {
    dcomp_alpha_probe_->Shutdown();
    dcomp_alpha_probe_ = nullptr;
  }
  if (dcomp_probe_hwnd_) {
    DestroyWindow(dcomp_probe_hwnd_);
    dcomp_probe_hwnd_ = nullptr;
  }

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
    case WM_SIZE:
      if (dcomp_probe_hwnd_) {
        RECT rect = GetClientArea();
        HWND flutter_hwnd = flutter_controller_->view()->GetNativeWindow();
        SetWindowPos(dcomp_probe_hwnd_, HWND_BOTTOM, rect.left, rect.top,
                     rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOACTIVATE);
        SetWindowPos(flutter_hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      }
      if (dcomp_alpha_probe_) {
        RECT rect = GetClientArea();
        dcomp_alpha_probe_->Resize(rect);
      }
      break;
    case WM_TIMER:
      if (dcomp_alpha_probe_) {
        dcomp_alpha_probe_->Render();
      }
      return 0;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}

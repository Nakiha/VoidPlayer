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

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_USE_HOSTBACKDROPBRUSH
#define DWMWA_USE_HOSTBACKDROPBRUSH 17
#endif

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

constexpr wchar_t kDCompProbeWindowClass[] = L"VOID_PLAYER_DCOMP_PROBE";
constexpr DWORD kDwmSystemBackdropMainWindow = 2;

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
  window_class.hbrBackground = nullptr;
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

void ApplyProbeMicaBackdrop(HWND hwnd, const char* stage) {
  MARGINS margins = {-1, -1, -1, -1};
  HRESULT extend_frame_hr = DwmExtendFrameIntoClientArea(hwnd, &margins);

  BOOL dark_mode = TRUE;
  HRESULT dark_hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                         &dark_mode, sizeof(dark_mode));

  BOOL host_backdrop = TRUE;
  HRESULT host_backdrop_hr =
      DwmSetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &host_backdrop,
                            sizeof(host_backdrop));

  DWORD backdrop = kDwmSystemBackdropMainWindow;
  HRESULT backdrop_hr =
      DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop,
                            sizeof(backdrop));

  WindowProbeLog(std::string(stage) + " ApplyProbeMicaBackdrop hwnd=" +
                 PtrString(hwnd) + " extendFrame=" +
                 HrString(extend_frame_hr) + " dark=" + HrString(dark_hr) +
                 " hostBackdropBrush=" + HrString(host_backdrop_hr) +
                 " systemBackdropMainWindow=" + HrString(backdrop_hr));
}

void SetProbeCloak(HWND hwnd, bool cloaked, const char* stage) {
  BOOL value = cloaked ? TRUE : FALSE;
  HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof(value));
  WindowProbeLog(std::string(stage) + " DwmCloak hwnd=" + PtrString(hwnd) +
                 " cloaked=" + std::to_string(cloaked ? 1 : 0) +
                 " hr=" + HrString(hr));
}

}  // namespace

FlutterWindow::FlutterWindow(const flutter::DartProject& project,
                             bool dcomp_alpha_probe,
                             bool dcomp_surface_probe,
                             bool dcomp_hdr_sdr_mix_probe)
    : project_(project),
      dcomp_alpha_probe_enabled_(dcomp_alpha_probe),
      dcomp_surface_probe_enabled_(dcomp_surface_probe),
      dcomp_hdr_sdr_mix_probe_enabled_(dcomp_hdr_sdr_mix_probe) {}

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
  const bool any_dcomp_probe =
      dcomp_alpha_probe_enabled_ || dcomp_surface_probe_enabled_ ||
      dcomp_hdr_sdr_mix_probe_enabled_;
  if (any_dcomp_probe) {
    ApplyProbeMicaBackdrop(GetHandle(), "OnCreate");
  }
  if (dcomp_alpha_probe_enabled_ || dcomp_hdr_sdr_mix_probe_enabled_) {
    RegisterDCompProbeWindowClass();
    RECT rect = GetClientArea();
    dcomp_probe_hwnd_ = CreateWindowEx(
        0, kDCompProbeWindowClass, L"", WS_CHILD | WS_VISIBLE, rect.left,
        rect.top, rect.right - rect.left, rect.bottom - rect.top, GetHandle(),
        nullptr, GetModuleHandle(nullptr), nullptr);
    dcomp_child_created = dcomp_probe_hwnd_ != nullptr;
  }

  SetChildContent(flutter_hwnd);

  if (dcomp_alpha_probe_enabled_ || dcomp_hdr_sdr_mix_probe_enabled_) {
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
  if (dcomp_surface_probe_enabled_) {
    LONG_PTR ex_style = GetWindowLongPtr(flutter_hwnd, GWL_EXSTYLE);
    SetWindowLongPtr(flutter_hwnd, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
    flutter_color_key_ok =
        SetLayeredWindowAttributes(flutter_hwnd, 0, 255, LWA_ALPHA);
    SetWindowPos(GetHandle(), nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    SetWindowPos(flutter_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
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
  if (dcomp_hdr_sdr_mix_probe_enabled_) {
    dcomp_alpha_probe_ = std::make_unique<DCompAlphaProbe>();
    native_probe_initialized =
        dcomp_alpha_probe_->InitializeWithSdrOverlay(
            dcomp_probe_hwnd_ ? dcomp_probe_hwnd_ : flutter_hwnd, GetHandle(),
            true);
    WindowProbeLog("hdrSdrMixProbe host=" + PtrString(GetHandle()) +
                   " flutter=" + PtrString(flutter_hwnd) +
                   " dcomp_child=" + PtrString(dcomp_probe_hwnd_) +
                   " flutterColorKey=" +
                   std::to_string(flutter_color_key_ok ? 1 : 0) +
                   " dcompChildCreated=" +
                   std::to_string(dcomp_child_created ? 1 : 0) +
                   " dcompChildPositioned=" +
                   std::to_string(dcomp_child_positioned ? 1 : 0) +
                   " nativeProbeInitialized=" +
                   std::to_string(native_probe_initialized ? 1 : 0));
  }
  if (dcomp_surface_probe_enabled_) {
    dcomp_alpha_probe_ = std::make_unique<DCompAlphaProbe>();
    native_probe_initialized = dcomp_alpha_probe_->InitializeWithFlutterSurface(
        GetHandle(), flutter_hwnd, GetHandle());
    if (native_probe_initialized) {
      SetProbeCloak(flutter_hwnd, true, "SurfaceProbe");
    }
    WindowProbeLog("surfaceProbe host=" + PtrString(GetHandle()) +
                   " flutter=" + PtrString(flutter_hwnd) +
                   " hostDwm=" + HrString(host_dwm_hr) +
                   " flutterDwm=" + HrString(flutter_dwm_hr) +
                   " flutterLayeredAlpha=" +
                   std::to_string(flutter_color_key_ok ? 1 : 0) +
                   " nativeProbeInitialized=" +
                   std::to_string(native_probe_initialized ? 1 : 0));
  }

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    if (dcomp_alpha_probe_enabled_ || dcomp_surface_probe_enabled_ ||
        dcomp_hdr_sdr_mix_probe_enabled_) {
      ApplyProbeMicaBackdrop(GetHandle(), "FirstFrame");
    }
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  if (dcomp_surface_probe_enabled_ && flutter_controller_ &&
      flutter_controller_->view()) {
    SetProbeCloak(flutter_controller_->view()->GetNativeWindow(), false,
                  "OnDestroy");
  }
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
      if (dcomp_surface_probe_enabled_ && flutter_controller_ &&
          flutter_controller_->view()) {
        RECT rect = GetClientArea();
        HWND flutter_hwnd = flutter_controller_->view()->GetNativeWindow();
        MoveWindow(flutter_hwnd, rect.left, rect.top, rect.right - rect.left,
                   rect.bottom - rect.top, TRUE);
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

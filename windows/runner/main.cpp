#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <flutter_windows.h>
#include <windows.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>

#include "flutter_window.h"
#include "startup_trace.h"
#include "utils.h"

namespace {

struct RestoredWindowBounds {
  int x;
  int y;
  int width;
  int height;
};

std::filesystem::path GetConfigPath() {
  wchar_t executable_path[MAX_PATH];
  DWORD length = GetModuleFileNameW(nullptr, executable_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return L"config.json";
  }
  return std::filesystem::path(executable_path).parent_path() / L"config.json";
}

std::optional<double> ExtractJsonNumber(const std::string& json,
                                        const char* key) {
  const std::regex pattern(std::string("\"") + key +
                           R"("\s*:\s*(-?\d+(?:\.\d+)?))");
  std::smatch match;
  if (!std::regex_search(json, match, pattern) || match.size() < 2) {
    return std::nullopt;
  }
  try {
    return std::stod(match[1].str());
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> ExtractWindowJson(const std::string& json) {
  const size_t key = json.find("\"window\"");
  if (key == std::string::npos) {
    return std::nullopt;
  }
  const size_t open = json.find('{', key);
  if (open == std::string::npos) {
    return std::nullopt;
  }
  const size_t close = json.find('}', open);
  if (close == std::string::npos || close <= open) {
    return std::nullopt;
  }
  return json.substr(open, close - open + 1);
}

bool IsOnScreen(const RestoredWindowBounds& bounds) {
  RECT rect = {bounds.x, bounds.y, bounds.x + bounds.width,
               bounds.y + bounds.height};
  return MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != nullptr;
}

std::optional<RestoredWindowBounds> LoadRestoredWindowBounds() {
  std::ifstream file(GetConfigPath());
  if (!file) {
    return std::nullopt;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  const auto window_json = ExtractWindowJson(buffer.str());
  if (!window_json) {
    return std::nullopt;
  }

  const auto x = ExtractJsonNumber(*window_json, "x");
  const auto y = ExtractJsonNumber(*window_json, "y");
  const auto width = ExtractJsonNumber(*window_json, "width");
  const auto height = ExtractJsonNumber(*window_json, "height");
  if (!x || !y || !width || !height || *width < 520 || *height < 360) {
    return std::nullopt;
  }

  RestoredWindowBounds bounds = {
      static_cast<int>(std::round(*x)),
      static_cast<int>(std::round(*y)),
      static_cast<int>(std::round(*width)),
      static_cast<int>(std::round(*height)),
  };
  if (!IsOnScreen(bounds)) {
    return std::nullopt;
  }
  return bounds;
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  RunnerStartupTraceReset();

  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
    // Attached to parent console — sync Dart VM stdout/stderr so
    // dart:io stdout.writeln() actually reaches the terminal.
    FlutterDesktopResyncOutputStreams();
  } else if (::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }
  RunnerStartupTraceMark("console ready");

  // Initialize COM, so that it is available for use in the library and/or
  // plugins.
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  RunnerStartupTraceMark("COM initialized");

  flutter::DartProject project(L"data");
  RunnerStartupTraceMark("Dart project created");

  std::vector<std::string> command_line_arguments =
      GetCommandLineArguments();

  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));
  RunnerStartupTraceMark("entrypoint arguments set");

  FlutterWindow window(project);
  const auto restored_bounds = LoadRestoredWindowBounds();
  RunnerStartupTraceMark(restored_bounds ? "restored bounds loaded"
                                         : "restored bounds unavailable");
  const bool created = restored_bounds
                           ? window.CreateWithBounds(
                                 L"Void Player", restored_bounds->x,
                                 restored_bounds->y, restored_bounds->width,
                                 restored_bounds->height)
                           : window.Create(L"Void Player",
                                           Win32Window::Point(10, 10),
                                           Win32Window::Size(1280, 720));
  RunnerStartupTraceMark("window created");
  if (!created) {
    return EXIT_FAILURE;
  }
  window.SetQuitOnClose(true);
  RunnerStartupTraceMark("message loop starting");

  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
  }

  ::CoUninitialize();
  return EXIT_SUCCESS;
}

#include "macos/player/native_player_bridge.h"

#include <unistd.h>

#include <filesystem>
#include <iostream>

int main() {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("voidplayer_macos_crash_handler_" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::cerr << "failed to create crash handler temp dir: " << ec.message() << "\n";
    return 1;
  }

  VPMacOSInstallCrashHandler(dir.string().c_str());
  VPMacOSInstallCrashHandler(dir.string().c_str());
  VPMacOSRemoveCrashHandler();
  VPMacOSRemoveCrashHandler();

  std::filesystem::remove_all(dir, ec);
  return 0;
}

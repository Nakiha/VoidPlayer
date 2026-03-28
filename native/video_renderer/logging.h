#pragma once
#include <string>
#include <functional>

namespace vr {

struct LogConfig {
    /// Custom spdlog format pattern string.
    /// Default: "[%Y-%m-%d %H:%M:%S.%e] [%l] %v"
    std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] %v";

    /// File path for log output. Empty = no file logging.
    std::string file_path;

    /// Maximum log file size in bytes (default 5MB). 0 = unlimited.
    size_t max_file_size = 5 * 1024 * 1024;

    /// Number of rotated log files to keep (default 3). 0 = no rotation.
    int max_files = 3;

    /// Minimum log level. 0=trace, 1=debug, 2=info, 3=warn, 4=error, 5=critical, 6=off.
    int level = 2;
};

/// Configure spdlog with custom format, file sink, and level.
/// Must be called before any other vr:: operations (before Renderer::initialize).
/// Can be called multiple times to reconfigure.
void configure_logging(const LogConfig& config);

/// Install Windows crash handler (SEH).
/// Writes crash dump to crash_dir/{timestamp}_crash.log.
/// If crash_dir is empty, no file is written (only output to stderr if available).
void install_crash_handler(const std::string& crash_dir);

/// Remove crash handler. Called automatically in atexit or DLL unload.
void remove_crash_handler();

} // namespace vr

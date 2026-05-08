#pragma once
#include <string>
#include <spdlog/common.h>

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

    /// Minimum log level.
    spdlog::level::level_enum level = spdlog::level::info;
};

/// Configure native logging with custom format, file sink, and level.
/// Existing host/default spdlog sinks are preserved; only sinks previously
/// owned by VoidPlayer native are replaced on reconfigure.
/// Must be called before any other vr:: operations (before Renderer::initialize).
/// Can be called multiple times to reconfigure.
void configure_logging(const LogConfig& config);

} // namespace vr

#pragma once

#include <string>

namespace vr {

struct WindowsCrashHandlerConfig {
    /// Directory for crash text logs. Empty means no crash file is written.
    std::string crash_dir;

    /// Install SetUnhandledExceptionFilter. Process-global; host code may
    /// replace it later.
    bool install_unhandled_exception_filter = true;

    /// Install a first-chance vectored handler. Process-global and harder for
    /// other frameworks to replace.
    bool install_vectored_exception_handler = true;

    /// Route CRT purecall / invalid-parameter failures into the same crash log.
    bool install_crt_handlers = true;
};

/// Install process-wide Windows crash diagnostics.
/// This is intentionally separate from logging and renderer ownership: the
/// hooks affect the whole process, not just one Renderer instance.
bool install_windows_crash_handler(const WindowsCrashHandlerConfig& config);

/// Remove hooks installed by install_windows_crash_handler().
void remove_windows_crash_handler();

/// True when this module currently owns process-wide crash hooks.
bool windows_crash_handler_installed();

/// Legacy API kept for Python/FFI callers.
void install_crash_handler(const std::string& crash_dir);
void remove_crash_handler();

} // namespace vr

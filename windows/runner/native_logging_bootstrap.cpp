#include "native_logging_bootstrap.h"

#include "common/win_utf8.h"
#include "windows/common/windows_crash_handler.h"
#include "common/logging.h"
#include "startup_trace.h"

#include <spdlog/spdlog.h>
#include <windows.h>
#include <cwchar>
#include <filesystem>
#include <sstream>

namespace {

std::string get_exe_dir() {
    return vr::win_utf8::module_directory_utf8();
}

bool directory_exists_utf8(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(vr::win_utf8::path_from_utf8(path), ec);
}

std::string default_app_data_root() {
    const std::string exe_dir = get_exe_dir();
    if (directory_exists_utf8(exe_dir + "\\cache")) {
        return exe_dir;
    }
    std::string app_data = vr::win_utf8::get_env_utf8(L"APPDATA");
    if (app_data.empty()) {
        app_data = vr::win_utf8::get_env_utf8(L"LOCALAPPDATA");
    }
    if (!app_data.empty()) {
        return app_data + "\\VoidPlayer";
    }
    return exe_dir + "\\VoidPlayer";
}

std::string sanitize_log_file_name(std::string name) {
    if (name.empty()) return name;
    for (auto& ch : name) {
        switch (ch) {
        case '\\':
        case '/':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            ch = '_';
            break;
        default:
            break;
        }
    }
    return name;
}

std::string current_process_role() {
    const wchar_t* command_line = GetCommandLineW();
    if (command_line && wcsstr(command_line, L"--standalone-analysis") != nullptr) {
        return "analysis";
    }
    return "main";
}

std::string default_native_log_file_name() {
    std::ostringstream name;
    name << "native_" << current_process_role() << "_" << GetCurrentProcessId() << ".log";
    return name.str();
}

} // namespace

NativeLoggingBootstrapResult NativeLoggingBootstrap::InitializeDefaults() {
    logs_dir_ = default_app_data_root() + "\\logs";
    log_file_name_ = default_native_log_file_name();
    auto result = Configure("info");

    RunnerStartupTraceFlush();
    vr::WindowsCrashHandlerConfig crash_config;
    crash_config.crash_dir = logs_dir_;
    crash_config.install_unhandled_exception_filter = true;
    crash_config.install_vectored_exception_handler = true;
    crash_config.install_crt_handlers = true;
    vr::install_windows_crash_handler(crash_config);

    return result;
}

NativeLoggingBootstrapResult NativeLoggingBootstrap::Reconfigure(
    std::string level,
    std::string logs_dir,
    std::string log_file_name) {
    EnsureDefaultPaths();
    if (!logs_dir.empty()) {
        logs_dir_ = logs_dir;
    }
    log_file_name = sanitize_log_file_name(log_file_name);
    if (!log_file_name.empty()) {
        log_file_name_ = log_file_name;
    }
    return Configure(level);
}

void NativeLoggingBootstrap::EnsureDefaultPaths() {
    if (logs_dir_.empty()) {
        logs_dir_ = default_app_data_root() + "\\logs";
    }
    if (log_file_name_.empty()) {
        log_file_name_ = default_native_log_file_name();
    }
}

NativeLoggingBootstrapResult NativeLoggingBootstrap::Configure(std::string level) {
    EnsureDefaultPaths();
    const auto parsed_level = spdlog::level::from_str(level);

    vr::LogConfig config;
    config.file_path = logs_dir_ + "\\" + log_file_name_;
    config.level = parsed_level;
    config.max_files = 5;
    config.configure_console_codepage = true;
    config.use_environment_level_override = true;
    config.manage_global_flush = true;
    vr::configure_logging(config);

    return NativeLoggingBootstrapResult{
        level,
        config.file_path,
        logs_dir_,
    };
}

#ifdef _WIN32
#ifndef SPDLOG_WCHAR_TO_UTF8_SUPPORT
#define SPDLOG_WCHAR_TO_UTF8_SUPPORT
#endif
#ifndef SPDLOG_UTF8_TO_WCHAR_CONSOLE
#define SPDLOG_UTF8_TO_WCHAR_CONSOLE
#endif
#endif

#include "common/logging.h"
#include "common/win_utf8.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/pattern_formatter.h>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>
#else
#include <spdlog/sinks/basic_file_sink.h>
#endif

namespace vr {

#ifdef _WIN32

static void configure_windows_utf8_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

static std::string windows_error_message(const char* operation, DWORD error) {
    return std::string(operation) + " failed, GetLastError=" + std::to_string(error);
}

static void ensure_parent_directory(const std::wstring& path) {
    std::error_code ec;
    std::filesystem::path fs_path(path);
    const auto parent = fs_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
}

static bool write_all(HANDLE handle, const void* data, DWORD size) {
    const auto* bytes = static_cast<const BYTE*>(data);
    DWORD total = 0;
    while (total < size) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes + total, size - total, &written, nullptr)) {
            return false;
        }
        if (written == 0) {
            return false;
        }
        total += written;
    }
    return true;
}

static void ensure_utf8_bom(HANDLE handle) {
    static constexpr BYTE kBom[] = {0xEF, 0xBB, 0xBF};
    static constexpr LONGLONG kMaxMigratedLogBytes = 64ll * 1024ll * 1024ll;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        return;
    }

    LARGE_INTEGER zero{};
    SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN);

    if (size.QuadPart == 0) {
        write_all(handle, kBom, static_cast<DWORD>(sizeof(kBom)));
        return;
    }

    BYTE prefix[sizeof(kBom)] = {};
    DWORD read = 0;
    if (!ReadFile(handle, prefix, static_cast<DWORD>(sizeof(prefix)), &read, nullptr)) {
        return;
    }
    if (read == sizeof(kBom) && std::memcmp(prefix, kBom, sizeof(kBom)) == 0) {
        return;
    }

    // Migrate ordinary existing logs once so Windows editors stop guessing ANSI.
    if (size.QuadPart > kMaxMigratedLogBytes) {
        return;
    }

    std::vector<BYTE> existing(static_cast<size_t>(size.QuadPart));
    SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN);
    DWORD existing_read = 0;
    if (!existing.empty() &&
        !ReadFile(handle, existing.data(), static_cast<DWORD>(existing.size()), &existing_read, nullptr)) {
        return;
    }
    if (static_cast<size_t>(existing_read) != existing.size()) {
        return;
    }

    SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN);
    if (!write_all(handle, kBom, static_cast<DWORD>(sizeof(kBom)))) {
        return;
    }
    if (!existing.empty() &&
        !write_all(handle, existing.data(), static_cast<DWORD>(existing.size()))) {
        return;
    }
    SetEndOfFile(handle);
}

template <typename Mutex>
class utf8_file_sink final : public spdlog::sinks::base_sink<Mutex> {
public:
    utf8_file_sink(const std::string& path, size_t max_file_size, int max_files)
        : path_(win_utf8::utf16_from_utf8(path))
        , max_file_size_(max_file_size)
        , max_files_(max_files) {
        if (path_.empty()) {
            throw spdlog::spdlog_ex("UTF-8 log path is empty or invalid");
        }
        open_file(false);
    }

    ~utf8_file_sink() override {
        if (file_ != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(file_);
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);
        if (formatted.size() > 0) {
            rotate_if_needed(formatted.size());
            write_all(file_, formatted.data(), static_cast<DWORD>(formatted.size()));
        }
    }

    void flush_() override {
        FlushFileBuffers(file_);
    }

private:
    std::wstring rotated_path(int index) const {
        return path_ + L"." + std::to_wstring(index);
    }

    void open_file(bool truncate) {
        ensure_parent_directory(path_);
        file_ = CreateFileW(
            path_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            throw spdlog::spdlog_ex(windows_error_message("CreateFileW", GetLastError()));
        }

        ensure_utf8_bom(file_);
        LARGE_INTEGER end{};
        SetFilePointerEx(file_, end, nullptr, FILE_END);
    }

    uint64_t current_size() const {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size) || size.QuadPart < 0) {
            return 0;
        }
        return static_cast<uint64_t>(size.QuadPart);
    }

    void rotate_if_needed(size_t incoming_bytes) {
        if (max_file_size_ == 0 || max_files_ <= 0 || file_ == INVALID_HANDLE_VALUE) {
            return;
        }
        if (current_size() + incoming_bytes <= max_file_size_) {
            return;
        }

        FlushFileBuffers(file_);
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;

        std::error_code ec;
        if (max_files_ <= 1) {
            std::filesystem::remove(path_, ec);
            open_file(true);
            return;
        }

        std::filesystem::remove(rotated_path(max_files_ - 1), ec);
        for (int i = max_files_ - 2; i >= 1; --i) {
            std::filesystem::rename(rotated_path(i), rotated_path(i + 1), ec);
            ec.clear();
        }
        std::filesystem::rename(path_, rotated_path(1), ec);
        open_file(true);
    }

    std::wstring path_;
    size_t max_file_size_ = 0;
    int max_files_ = 0;
    HANDLE file_ = INVALID_HANDLE_VALUE;
};

using utf8_file_sink_mt = utf8_file_sink<std::mutex>;

#endif

// Check if stderr is available (safe for Windows GUI apps)
static bool stderr_available() {
#ifdef _WIN32
    // GetStdHandle returns NULL for GUI apps without console
    // or if stderr was redirected to NUL
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    // Check it's not /dev/null equivalent
    DWORD type = GetFileType(h);
    if (type == FILE_TYPE_UNKNOWN) return false;
    return true;
#else
    return true;
#endif
}

void configure_logging(const LogConfig& config) {
#ifdef _WIN32
    if (config.configure_console_codepage) {
        configure_windows_utf8_console();
    }
#endif

    auto logger = spdlog::default_logger();
    static std::mutex logging_mutex;
    static std::vector<spdlog::sink_ptr> native_sinks;
    std::lock_guard<std::mutex> lock(logging_mutex);

    spdlog::level::level_enum effective_level = config.level;
    if (config.use_environment_level_override) {
        const char* env_level = std::getenv("VOIDPLAYER_NATIVE_LOG_LEVEL");
        if (env_level && env_level[0] != '\0') {
            auto parsed = spdlog::level::from_str(env_level);
            if (parsed != spdlog::level::off ||
                spdlog::level::to_string_view(spdlog::level::off) == env_level) {
                effective_level = parsed;
            }
        }
    }

    auto& logger_sinks = logger->sinks();
    logger_sinks.erase(
        std::remove_if(
            logger_sinks.begin(),
            logger_sinks.end(),
            [](const spdlog::sink_ptr& sink) {
                return std::find(native_sinks.begin(), native_sinks.end(), sink) !=
                    native_sinks.end();
            }),
        logger_sinks.end());
    native_sinks.clear();

    // Add file sink if path specified
    if (!config.file_path.empty()) {
        try {
#ifdef _WIN32
            auto file_sink = std::make_shared<utf8_file_sink_mt>(
                config.file_path, config.max_file_size, config.max_files);
#else
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                config.file_path, false);
#endif
            file_sink->set_pattern(config.pattern);
            file_sink->set_level(effective_level);
            native_sinks.push_back(file_sink);
            logger_sinks.push_back(std::move(file_sink));
        } catch (const spdlog::spdlog_ex& ex) {
            // If file sink fails, we still want other sinks
            // Use a temporary stderr to report the error
            if (stderr_available()) {
                fprintf(stderr, "[vr::logging] Failed to create file sink: %s\n", ex.what());
            }
        }
    }

    // Add stderr sink if available (safe for GUI apps — stderr_available
    // returns false when no console is attached, so no crash risk).
    // Uses the same level as the configured level.
    if (stderr_available() && logger_sinks.empty()) {
        auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        console_sink->set_pattern(config.pattern);
        console_sink->set_level(effective_level);
        native_sinks.push_back(console_sink);
        logger_sinks.push_back(std::move(console_sink));
    }

    // If no sinks at all (no file, no stderr), create a null-like setup
    // spdlog requires at least one sink, so we keep a file sink to /dev/null
    // or just use the empty logger (spdlog handles this gracefully by dropping messages)
    if (logger->sinks().empty()) {
        // No sinks available — logging is effectively disabled
        // spdlog will still accept log calls but output goes nowhere
    }

    // spdlog free functions still pass through the default logger. Keep host
    // sinks intact, but lower the logger threshold if needed so native-owned
    // sinks can receive the requested level.
    if (logger->level() > effective_level) {
        logger->set_level(effective_level);
    }

    if (config.manage_global_flush) {
        // Flush every info log so crash-adjacent traces are persisted to disk.
        logger->flush_on(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);

        // Keep a background flush as a fallback for sinks that buffer internally.
        spdlog::flush_every(std::chrono::seconds(1));
    }
}

} // namespace vr

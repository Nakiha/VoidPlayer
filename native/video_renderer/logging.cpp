#include "video_renderer/logging.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/pattern_formatter.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <atomic>
#endif

namespace vr {

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
    auto logger = spdlog::default_logger();

    // Remove all existing sinks
    logger->sinks().clear();

    // Add file sink if path specified
    if (!config.file_path.empty()) {
        try {
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                config.file_path, false);
            file_sink->set_pattern(config.pattern);
            logger->sinks().push_back(std::move(file_sink));
        } catch (const spdlog::spdlog_ex& ex) {
            // If file sink fails, we still want other sinks
            // Use a temporary stderr to report the error
            if (stderr_available()) {
                fprintf(stderr, "[vr::logging] Failed to create file sink: %s\n", ex.what());
            }
        }
    }

    // Add stderr sink if available (safe for GUI apps)
    if (stderr_available()) {
        auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        console_sink->set_pattern(config.pattern);
        logger->sinks().push_back(std::move(console_sink));
    }

    // If no sinks at all (no file, no stderr), create a null-like setup
    // spdlog requires at least one sink, so we keep a file sink to /dev/null
    // or just use the empty logger (spdlog handles this gracefully by dropping messages)
    if (logger->sinks().empty()) {
        // No sinks available — logging is effectively disabled
        // spdlog will still accept log calls but output goes nowhere
    }

    // Set level
    auto level = static_cast<spdlog::level::level_enum>(config.level);
    logger->set_level(level);
    spdlog::set_level(level);

    // Flush on warn and above
    logger->flush_on(spdlog::level::warn);
    spdlog::flush_on(spdlog::level::warn);

    // Flush every 3 seconds
    spdlog::flush_every(std::chrono::seconds(3));
}

// ============================================================
// Crash Handler (Windows SEH)
// ============================================================

#ifdef _WIN32

static std::string g_crash_dir;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;

// Async-signal-safe integer to string conversion
static int safe_itoa(int64_t value, char* buf, int bufsize) {
    if (bufsize < 2) return 0;
    if (value == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }

    bool neg = value < 0;
    if (neg) value = -value;

    char tmp[32];
    int pos = 0;
    while (value > 0 && pos < 31) {
        tmp[pos++] = '0' + static_cast<char>(value % 10);
        value /= 10;
    }

    int written = 0;
    if (neg && written < bufsize - 1) buf[written++] = '-';

    for (int i = pos - 1; i >= 0 && written < bufsize - 1; --i) {
        buf[written++] = tmp[i];
    }
    buf[written] = '\0';
    return written;
}

// Async-signal-safe string length
static int safe_strlen(const char* s) {
    int len = 0;
    while (s && s[len]) ++len;
    return len;
}

// Async-signal-safe string append to buffer
static int safe_append(char* buf, int pos, int maxsize, const char* str) {
    int len = safe_strlen(str);
    for (int i = 0; i < len && pos < maxsize - 1; ++i) {
        buf[pos++] = str[i];
    }
    buf[pos] = '\0';
    return pos;
}

// Write crash info to a file handle (async-signal-safe)
static void write_crash_log(HANDLE hFile, const char* content, int len) {
    DWORD written = 0;
    WriteFile(hFile, content, static_cast<DWORD>(len), &written, nullptr);
}

// Get current timestamp for crash filename (async-signal-safe)
static void get_timestamp_filename(char* buf, int bufsize) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    int pos = 0;
    pos = safe_append(buf, pos, bufsize, "crash_");

    char num[16];
    safe_itoa(st.wYear, num, 16); pos = safe_append(buf, pos, bufsize, num);
    buf[pos++] = '-';
    if (st.wMonth < 10) buf[pos++] = '0';
    safe_itoa(st.wMonth, num, 16); pos = safe_append(buf, pos, bufsize, num);
    buf[pos++] = '-';
    if (st.wDay < 10) buf[pos++] = '0';
    safe_itoa(st.wDay, num, 16); pos = safe_append(buf, pos, bufsize, num);
    buf[pos++] = '_';
    if (st.wHour < 10) buf[pos++] = '0';
    safe_itoa(st.wHour, num, 16); pos = safe_append(buf, pos, bufsize, num);
    if (st.wMinute < 10) buf[pos++] = '0';
    safe_itoa(st.wMinute, num, 16); pos = safe_append(buf, pos, bufsize, num);
    if (st.wSecond < 10) buf[pos++] = '0';
    safe_itoa(st.wSecond, num, 16); pos = safe_append(buf, pos, bufsize, num);
    pos = safe_append(buf, pos, bufsize, ".log");
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    // Build crash log content in a stack buffer (async-signal-safe)
    char logbuf[4096];
    int pos = 0;

    pos = safe_append(logbuf, pos, sizeof(logbuf),
        "=== VoidPlayer Native Crash ===\n");

    // Timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[64];
    int tsp = 0;
    tsp = safe_append(ts, tsp, sizeof(ts), "Time: ");
    char num[16];
    safe_itoa(st.wYear, num, 16); tsp = safe_append(ts, tsp, sizeof(ts), num);
    ts[tsp++] = '-';
    if (st.wMonth < 10) ts[tsp++] = '0';
    safe_itoa(st.wMonth, num, 16); tsp = safe_append(ts, tsp, sizeof(ts), num);
    ts[tsp++] = '-';
    if (st.wDay < 10) ts[tsp++] = '0';
    safe_itoa(st.wDay, num, 16); tsp = safe_append(ts, tsp, sizeof(ts), num);
    ts[tsp++] = ' ';
    if (st.wHour < 10) ts[tsp++] = '0';
    safe_itoa(st.wHour, num, 16); tsp = safe_append(ts, tsp, sizeof(ts), num);
    ts[tsp++] = ':';
    if (st.wMinute < 10) ts[tsp++] = '0';
    safe_itoa(st.wMinute, num, 16); tsp = safe_append(ts, tsp, sizeof(ts), num);
    ts[tsp++] = ':';
    if (st.wSecond < 10) ts[tsp++] = '0';
    safe_itoa(st.wSecond, num, 16); tsp = safe_append(ts, tsp, sizeof(ts), num);
    ts[tsp] = '\0';
    pos = safe_append(logbuf, pos, sizeof(logbuf), ts);
    pos = safe_append(logbuf, pos, sizeof(logbuf), "\n");

    // Exception code
    if (ep && ep->ExceptionRecord) {
        pos = safe_append(logbuf, pos, sizeof(logbuf), "Exception code: 0x");
        char hex[20];
        // Manual hex conversion (async-signal-safe)
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        for (int i = 7; i >= 0; --i) {
            int digit = (code >> (i * 4)) & 0xF;
            hex[7 - i] = static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10);
        }
        hex[8] = '\0';
        pos = safe_append(logbuf, pos, sizeof(logbuf), hex);

        pos = safe_append(logbuf, pos, sizeof(logbuf), " at address: 0x");
        // Convert address to hex
        uintptr_t addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        for (int i = static_cast<int>(sizeof(uintptr_t) * 2 - 1); i >= 0; --i) {
            int digit = static_cast<int>((addr >> (i * 4)) & 0xF);
            hex[sizeof(uintptr_t) * 2 - 1 - i] = static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10);
        }
        hex[sizeof(uintptr_t) * 2] = '\0';
        pos = safe_append(logbuf, pos, sizeof(logbuf), hex);
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\n");

        // Exception flags
        pos = safe_append(logbuf, pos, sizeof(logbuf), "Flags: 0x");
        DWORD flags = ep->ExceptionRecord->ExceptionFlags;
        for (int i = 7; i >= 0; --i) {
            int digit = (flags >> (i * 4)) & 0xF;
            hex[7 - i] = static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10);
        }
        hex[8] = '\0';
        pos = safe_append(logbuf, pos, sizeof(logbuf), hex);
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\n");
    }

    // Register dump (x64)
    if (ep && ep->ContextRecord) {
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\nRegisters:\n");

#if defined(_M_X64) || defined(__x86_64__)
        pos = safe_append(logbuf, pos, sizeof(logbuf), "  RAX: 0x");
        auto reg_to_hex = [&](uintptr_t val) {
            char h[20];
            for (int i = static_cast<int>(sizeof(uintptr_t) * 2 - 1); i >= 0; --i) {
                int d = static_cast<int>((val >> (i * 4)) & 0xF);
                h[sizeof(uintptr_t) * 2 - 1 - i] = static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10);
            }
            h[sizeof(uintptr_t) * 2] = '\0';
            return safe_append(logbuf, pos, sizeof(logbuf), h);
        };
        pos = reg_to_hex(ep->ContextRecord->Rax);
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\n  RBX: 0x"); pos = reg_to_hex(ep->ContextRecord->Rbx);
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\n  RCX: 0x"); pos = reg_to_hex(ep->ContextRecord->Rcx);
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\n  RDX: 0x"); pos = reg_to_hex(ep->ContextRecord->Rdx);
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\n  RSI: 0x"); pos = reg_to_hex(ep->ContextRecord->Rsi);
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\n  RDI: 0x"); pos = reg_to_hex(ep->ContextRecord->Rdi);
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\n  RBP: 0x"); pos = reg_to_hex(ep->ContextRecord->Rbp);
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\n  RSP: 0x"); pos = reg_to_hex(ep->ContextRecord->Rsp);
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\n  RIP: 0x"); pos = reg_to_hex(ep->ContextRecord->Rip);
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\n");
#endif
    }

    pos = safe_append(logbuf, pos, sizeof(logbuf), "\n=== End Crash Log ===\n");

    // Write to file (async-signal-safe: CreateFileA + WriteFile)
    if (!g_crash_dir.empty()) {
        char filepath[MAX_PATH];
        int fppos = safe_append(filepath, 0, sizeof(filepath), g_crash_dir.c_str());
        fppos = safe_append(filepath, fppos, sizeof(filepath), "\\");
        char tsname[128];
        get_timestamp_filename(tsname, sizeof(tsname));
        fppos = safe_append(filepath, fppos, sizeof(filepath), tsname);

        HANDLE hFile = CreateFileA(
            filepath,
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (hFile != INVALID_HANDLE_VALUE) {
            write_crash_log(hFile, logbuf, pos);
            CloseHandle(hFile);
        }
    }

    // Write to stderr if available
    if (stderr_available()) {
        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        if (hErr != nullptr && hErr != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(hErr, logbuf, static_cast<DWORD>(pos), &written, nullptr);
        }
    }

    // Call previous handler if any
    if (g_prev_filter) {
        return g_prev_filter(ep);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

#endif // _WIN32

void install_crash_handler(const std::string& crash_dir) {
#ifdef _WIN32
    g_crash_dir = crash_dir;
    g_prev_filter = SetUnhandledExceptionFilter(crash_handler);

    // Also handle pure virtual calls and invalid parameters
    _set_purecall_handler([] {
        if (stderr_available()) {
            const char* msg = "[CRASH] Pure virtual function call\n";
            fprintf(stderr, "%s", msg);
        }
        // Trigger crash handler via intentional exception
        RaiseException(0xE06D7363, 0, 0, nullptr);
    });

    _set_invalid_parameter_handler([](
        const wchar_t* expression,
        const wchar_t* function,
        const wchar_t* file,
        unsigned int line,
        uintptr_t reserved
    ) {
        if (stderr_available()) {
            fprintf(stderr, "[CRASH] Invalid parameter: line %u\n", line);
        }
        RaiseException(0xE06D7363, 0, 0, nullptr);
    });
#endif
}

void remove_crash_handler() {
#ifdef _WIN32
    if (g_prev_filter) {
        SetUnhandledExceptionFilter(g_prev_filter);
        g_prev_filter = nullptr;
    }
    g_crash_dir.clear();
#endif
}

} // namespace vr

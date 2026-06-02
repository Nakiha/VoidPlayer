#ifdef _WIN32
#ifndef SPDLOG_WCHAR_TO_UTF8_SUPPORT
#define SPDLOG_WCHAR_TO_UTF8_SUPPORT
#endif
#ifndef SPDLOG_UTF8_TO_WCHAR_CONSOLE
#define SPDLOG_UTF8_TO_WCHAR_CONSOLE
#endif
#endif

#include "windows/common/windows_crash_handler.h"
#include "common/win_utf8.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace vr {

// ============================================================
// Crash Handler (Windows SEH)
// ============================================================

#ifdef _WIN32

static bool crash_stderr_available() {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    DWORD type = GetFileType(h);
    if (type == FILE_TYPE_UNKNOWN) return false;
    return true;
}

static std::string g_crash_dir;
static std::mutex g_crash_mutex;
static bool g_installed = false;
static bool g_unhandled_filter_installed = false;
static bool g_crt_handlers_installed = false;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;
static void* g_vectored_handler = nullptr;
static bool g_sym_initialized = false;
static _purecall_handler g_prev_purecall_handler = nullptr;
static _invalid_parameter_handler g_prev_invalid_parameter_handler = nullptr;

// Static symbol buffer (large, but safe in crash handler since it's not on the stack)
static BYTE g_symbol_buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];

static std::string default_symbol_cache_dir(const std::string& crash_dir) {
    const char* env_cache = std::getenv("VOIDPLAYER_SYMBOL_CACHE");
    if (env_cache && env_cache[0] != '\0') {
        return env_cache;
    }

    const auto local_app_data = win_utf8::get_env_utf8(L"LOCALAPPDATA");
    if (!local_app_data.empty()) {
        return local_app_data + "\\VoidPlayer\\Symbols";
    }

    return crash_dir.empty() ? "symbols" : crash_dir + "\\symbols";
}

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
    // Use a static buffer — heap may be corrupted during a crash.
    // Safe in practice: only one crash handler runs at a time (process is dying).
    static char logbuf[16384];
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

    // Stack trace using DbgHelp
    pos = safe_append(logbuf, pos, sizeof(logbuf), "\nCrashing thread ID: ");
    {
        DWORD tid = GetCurrentThreadId();
        char tid_str[16];
        int tp = 0;
        if (tid == 0) { tid_str[tp++] = '0'; }
        else {
            char tmp[16]; int t = 0;
            while (tid > 0) { tmp[t++] = '0' + (tid % 10); tid /= 10; }
            while (t > 0) tid_str[tp++] = tmp[--t];
        }
        tid_str[tp] = '\0';
        pos = safe_append(logbuf, pos, sizeof(logbuf), tid_str);
        pos = safe_append(logbuf, pos, sizeof(logbuf), "\n");
    }
    pos = safe_append(logbuf, pos, sizeof(logbuf), "\nStack Trace:\n");

    if (g_sym_initialized) {
        HANDLE hProcess = GetCurrentProcess();
        void* stack[32];
        USHORT frames = CaptureStackBackTrace(2, 32, stack, nullptr);

        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(g_symbol_buf);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

        char hex[20];

        for (USHORT i = 0; i < frames; ++i) {
            // Frame index
            char frame_prefix[16];
            frame_prefix[0] = ' ';
            frame_prefix[1] = '#';
            int fp = 2;
            if (i < 10) {
                frame_prefix[fp++] = '0' + static_cast<char>(i);
            } else {
                frame_prefix[fp++] = '0' + static_cast<char>(i / 10);
                frame_prefix[fp++] = '0' + static_cast<char>(i % 10);
            }
            frame_prefix[fp++] = ' ';
            frame_prefix[fp] = '\0';
            pos = safe_append(logbuf, pos, sizeof(logbuf), frame_prefix);

            // Address
            pos = safe_append(logbuf, pos, sizeof(logbuf), "0x");
            uintptr_t addr = reinterpret_cast<uintptr_t>(stack[i]);
            for (int j = static_cast<int>(sizeof(uintptr_t) * 2 - 1); j >= 0; --j) {
                int d = static_cast<int>((addr >> (j * 4)) & 0xF);
                hex[sizeof(uintptr_t) * 2 - 1 - j] = static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10);
            }
            hex[sizeof(uintptr_t) * 2] = '\0';
            pos = safe_append(logbuf, pos, sizeof(logbuf), hex);

            // Symbol name
            DWORD64 disp64 = 0;
            if (SymFromAddr(hProcess, reinterpret_cast<DWORD64>(stack[i]), &disp64, symbol)) {
                pos = safe_append(logbuf, pos, sizeof(logbuf), "  ");
                pos = safe_append(logbuf, pos, sizeof(logbuf), symbol->Name);
                pos = safe_append(logbuf, pos, sizeof(logbuf), " + 0x");
                for (int j = 7; j >= 0; --j) {
                    int d = static_cast<int>((disp64 >> (j * 4)) & 0xF);
                    hex[7 - j] = static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10);
                }
                hex[8] = '\0';
                pos = safe_append(logbuf, pos, sizeof(logbuf), hex);

                // File/line info
                DWORD disp32 = 0;
                if (SymGetLineFromAddr64(hProcess, reinterpret_cast<DWORD64>(stack[i]), &disp32, &line)) {
                    pos = safe_append(logbuf, pos, sizeof(logbuf), "  at ");
                    pos = safe_append(logbuf, pos, sizeof(logbuf), line.FileName);
                    pos = safe_append(logbuf, pos, sizeof(logbuf), ":");
                    char lineno[16];
                    safe_itoa(line.LineNumber, lineno, 16);
                    pos = safe_append(logbuf, pos, sizeof(logbuf), lineno);
                }
            } else {
                pos = safe_append(logbuf, pos, sizeof(logbuf), "  <no symbol>");
            }
            pos = safe_append(logbuf, pos, sizeof(logbuf), "\n");
        }
    } else {
        // Fallback: just raw addresses without symbol resolution
        void* stack[32];
        USHORT frames = CaptureStackBackTrace(2, 32, stack, nullptr);
        char hex[20];

        for (USHORT i = 0; i < frames; ++i) {
            char frame_prefix[16];
            frame_prefix[0] = ' ';
            frame_prefix[1] = '#';
            int fp = 2;
            if (i < 10) frame_prefix[fp++] = '0' + static_cast<char>(i);
            else { frame_prefix[fp++] = '0' + static_cast<char>(i / 10); frame_prefix[fp++] = '0' + static_cast<char>(i % 10); }
            frame_prefix[fp++] = ' ';
            frame_prefix[fp] = '\0';
            pos = safe_append(logbuf, pos, sizeof(logbuf), frame_prefix);

            pos = safe_append(logbuf, pos, sizeof(logbuf), "0x");
            uintptr_t addr = reinterpret_cast<uintptr_t>(stack[i]);
            for (int j = static_cast<int>(sizeof(uintptr_t) * 2 - 1); j >= 0; --j) {
                int d = static_cast<int>((addr >> (j * 4)) & 0xF);
                hex[sizeof(uintptr_t) * 2 - 1 - j] = static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10);
            }
            hex[sizeof(uintptr_t) * 2] = '\0';
            pos = safe_append(logbuf, pos, sizeof(logbuf), hex);
            pos = safe_append(logbuf, pos, sizeof(logbuf), "\n");
        }
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
    if (crash_stderr_available()) {
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

/// Vectored Exception Handler — logs the crash but lets other handlers run.
/// This fires before SetUnhandledExceptionFilter and cannot be overridden.
static LONG WINAPI vectored_crash_handler(EXCEPTION_POINTERS* ep) {
    // Only handle true crashes (access violations, etc.), not normal exceptions
    if (ep && ep->ExceptionRecord) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION ||
            code == EXCEPTION_STACK_OVERFLOW ||
            code == EXCEPTION_ILLEGAL_INSTRUCTION ||
            code == EXCEPTION_PRIV_INSTRUCTION ||
            code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
            code == EXCEPTION_DATATYPE_MISALIGNMENT) {
            // For stack overflow: use minimal logging to avoid recursive overflow.
            // The main crash_handler allocates 16KB on stack which will trigger
            // another stack overflow if the stack is already exhausted.
            if (code == EXCEPTION_STACK_OVERFLOW) {
                // Minimal write — no large stack allocations
                if (!g_crash_dir.empty()) {
                    char filepath[MAX_PATH];
                    int fppos = 0;
                    fppos = safe_append(filepath, fppos, sizeof(filepath), g_crash_dir.c_str());
                    fppos = safe_append(filepath, fppos, sizeof(filepath), "\\crash_stack_overflow.log");
                    HANDLE hFile = CreateFileA(filepath, GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        const char* msg = "=== VoidPlayer Native Crash (Stack Overflow) ===\n"
                            "Exception: EXCEPTION_STACK_OVERFLOW\n"
                            "Full stack trace unavailable (stack too deep for safe capture)\n"
                            "=== End Crash Log ===\n";
                        DWORD written = 0;
                        WriteFile(hFile, msg, static_cast<DWORD>(strlen(msg)), &written, nullptr);
                        CloseHandle(hFile);
                    }
                }
                return EXCEPTION_CONTINUE_SEARCH;
            }
            // Reuse the main crash_handler for logging
            crash_handler(ep);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif // _WIN32

bool install_windows_crash_handler(const WindowsCrashHandlerConfig& config) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_crash_mutex);
    if (g_installed) return true;
    g_installed = true;

    g_crash_dir = config.crash_dir;

    // Pre-initialize DbgHelp symbol handler so crash handler can resolve symbols.
    // Must be done before the crash — SymInitialize is not safe in SEH context.
    HANDLE hProcess = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);

    // Build symbol search path: exe directory + crash directory + default
    std::string exe_dir = win_utf8::module_directory_utf8();

    std::string symbol_cache_dir = default_symbol_cache_dir(config.crash_dir);
    std::string sym_path = exe_dir + ";" + config.crash_dir + ";srv*" + symbol_cache_dir +
        "*https://msdl.microsoft.com/download/symbols";
    if (SymInitialize(hProcess, sym_path.c_str(), TRUE)) {
        g_sym_initialized = true;
        if (crash_stderr_available()) {
            fprintf(stderr, "[vr::crash] DbgHelp initialized, symbol path: %s\n", sym_path.c_str());
        }
    } else {
        // Retry without invading process (in case another lib already initialized)
        if (SymInitialize(hProcess, sym_path.c_str(), FALSE)) {
            g_sym_initialized = true;
        }
    }

    if (config.install_unhandled_exception_filter) {
        g_prev_filter = SetUnhandledExceptionFilter(crash_handler);
        g_unhandled_filter_installed = true;
    }

    // Also install Vectored Exception Handler as first-responder.
    // Unlike SetUnhandledExceptionFilter, VEH cannot be overridden by
    // frameworks (e.g. Flutter engine) that install their own handlers later.
    if (config.install_vectored_exception_handler) {
        g_vectored_handler = AddVectoredExceptionHandler(1 /* CALL_FIRST */, vectored_crash_handler);
    }

    // Also handle pure virtual calls and invalid parameters
    if (config.install_crt_handlers) {
        g_prev_purecall_handler = _set_purecall_handler([] {
            if (crash_stderr_available()) {
                const char* msg = "[CRASH] Pure virtual function call\n";
                fprintf(stderr, "%s", msg);
            }
            // Trigger crash handler via intentional exception.
            RaiseException(0xE06D7363, 0, 0, nullptr);
        });

        g_prev_invalid_parameter_handler = _set_invalid_parameter_handler([](
            const wchar_t* expression,
            const wchar_t* function,
            const wchar_t* file,
            unsigned int line,
            uintptr_t reserved
        ) {
            (void)expression;
            (void)function;
            (void)file;
            (void)reserved;
            if (crash_stderr_available()) {
                fprintf(stderr, "[CRASH] Invalid parameter: line %u\n", line);
            }
            RaiseException(0xE06D7363, 0, 0, nullptr);
        });
        g_crt_handlers_installed = true;
    }
    return true;
#else
    (void)config;
    return false;
#endif
}

void remove_windows_crash_handler() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_crash_mutex);
    if (!g_installed) return;

    if (g_vectored_handler) {
        RemoveVectoredExceptionHandler(g_vectored_handler);
        g_vectored_handler = nullptr;
    }
    if (g_unhandled_filter_installed) {
        SetUnhandledExceptionFilter(g_prev_filter);
        g_prev_filter = nullptr;
        g_unhandled_filter_installed = false;
    }
    if (g_crt_handlers_installed) {
        _set_purecall_handler(g_prev_purecall_handler);
        _set_invalid_parameter_handler(g_prev_invalid_parameter_handler);
        g_prev_purecall_handler = nullptr;
        g_prev_invalid_parameter_handler = nullptr;
        g_crt_handlers_installed = false;
    }
    if (g_sym_initialized) {
        SymCleanup(GetCurrentProcess());
        g_sym_initialized = false;
    }
    g_crash_dir.clear();
    g_installed = false;
#endif
}

bool windows_crash_handler_installed() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_crash_mutex);
    return g_installed;
#else
    return false;
#endif
}

void install_crash_handler(const std::string& crash_dir) {
    WindowsCrashHandlerConfig config;
    config.crash_dir = crash_dir;
    (void)install_windows_crash_handler(config);
}

void remove_crash_handler() {
    remove_windows_crash_handler();
}

} // namespace vr

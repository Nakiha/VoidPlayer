#pragma once

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace voidview {

// 异步安全的字符串长度
inline size_t SafeStrLen(const char* s) {
    size_t len = 0;
    while (s && s[len]) ++len;
    return len;
}

// 异步安全的整数转十六进制字符串
inline char* SafeU64ToHex(char* buf, size_t bufSize, unsigned __int64 val) {
    static const char hex[] = "0123456789ABCDEF";
    if (bufSize < 3) return buf;

    buf[0] = '0';
    buf[1] = 'x';

    // 从高位开始，找到第一个非零或确定最小位数
    int startNibble = 60;
    while (startNibble > 0 && ((val >> startNibble) & 0xF) == 0) {
        startNibble -= 4;
    }
    if (startNibble < 0) startNibble = 0;

    size_t pos = 2;
    for (int i = startNibble; i >= 0 && pos < bufSize - 1; i -= 4) {
        buf[pos++] = hex[(val >> i) & 0xF];
    }
    buf[pos] = '\0';
    return buf;
}

// 异步安全的整数转十进制字符串
inline char* SafeU32ToDec(char* buf, size_t bufSize, unsigned int val) {
    if (bufSize < 2) { buf[0] = '\0'; return buf; }

    char tmp[12];
    int pos = 0;
    if (val == 0) {
        tmp[pos++] = '0';
    } else {
        while (val > 0 && pos < 11) {
            tmp[pos++] = '0' + (val % 10);
            val /= 10;
        }
    }

    size_t outPos = 0;
    while (pos > 0 && outPos < bufSize - 1) {
        buf[outPos++] = tmp[--pos];
    }
    buf[outPos] = '\0';
    return buf;
}

// 异步安全的写入
inline void SafeWrite(HANDLE hFile, const char* str) {
    DWORD written;
    WriteFile(hFile, str, static_cast<DWORD>(SafeStrLen(str)), &written, nullptr);
}

inline void SafeWrite(HANDLE hFile, char ch) {
    DWORD written;
    WriteFile(hFile, &ch, 1, &written, nullptr);
}

inline LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exc) {
    // 获取时间 - 使用 Windows API，异步安全
    SYSTEMTIME st;
    GetLocalTime(&st);

    // 生成文件名
    char crash_file[MAX_PATH];
    {
        char year[8], month[4], day[4], hour[4], minute[4], second[4];
        SafeU32ToDec(year, sizeof(year), st.wYear);
        SafeU32ToDec(month, sizeof(month), st.wMonth);
        SafeU32ToDec(day, sizeof(day), st.wDay);
        SafeU32ToDec(hour, sizeof(hour), st.wHour);
        SafeU32ToDec(minute, sizeof(minute), st.wMinute);
        SafeU32ToDec(second, sizeof(second), st.wSecond);

        // 补零
        char month2[4] = {0}, day2[4] = {0}, hour2[4] = {0}, minute2[4] = {0}, second2[4] = {0};
        if (st.wMonth < 10) { month2[0] = '0'; month2[1] = month[0]; }
        else { month2[0] = month[0]; month2[1] = month[1]; }
        if (st.wDay < 10) { day2[0] = '0'; day2[1] = day[0]; }
        else { day2[0] = day[0]; day2[1] = day[1]; }
        if (st.wHour < 10) { hour2[0] = '0'; hour2[1] = hour[0]; }
        else { hour2[0] = hour[0]; hour2[1] = hour[1]; }
        if (st.wMinute < 10) { minute2[0] = '0'; minute2[1] = minute[0]; }
        else { minute2[0] = minute[0]; minute2[1] = minute[1]; }
        if (st.wSecond < 10) { second2[0] = '0'; second2[1] = second[0]; }
        else { second2[0] = second[0]; second2[1] = second[1]; }

        // 拼接: crash_YYYYMMDD_HHMMSS.log
        char* p = crash_file;
        const char* prefix = "crash_";
        while (*prefix) *p++ = *prefix++;
        const char* py = year; while (*py) *p++ = *py++;
        const char* pm = month2; while (*pm) *p++ = *pm++;
        const char* pd = day2; while (*pd) *p++ = *pd++;
        *p++ = '_';
        const char* ph = hour2; while (*ph) *p++ = *ph++;
        const char* pmin = minute2; while (*pmin) *p++ = *pmin++;
        const char* ps = second2; while (*ps) *p++ = *ps++;
        const char* suffix = ".log";
        while (*suffix) *p++ = *suffix++;
        *p = '\0';
    }

    // 使用 CreateFileW 而非 fopen
    // 需要转换到宽字符，但 MultiByteToWideChar 不安全
    // 直接用 ASCII 版本的 CreateFileA
    HANDLE hFile = CreateFileA(
        crash_file,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile != INVALID_HANDLE_VALUE) {
        char hexBuf[24];
        char decBuf[16];

        SafeWrite(hFile, "=== VoidView Native Crash Report ===\n");
        SafeWrite(hFile, "Time: ");
        SafeWrite(hFile, crash_file + 6);  // 跳过 "crash_" 前缀
        SafeWrite(hFile, "\n");

        SafeWrite(hFile, "Exception Code: ");
        SafeWrite(hFile, SafeU64ToHex(hexBuf, sizeof(hexBuf), exc->ExceptionRecord->ExceptionCode));
        SafeWrite(hFile, "\n");

        SafeWrite(hFile, "Exception Address: ");
        SafeWrite(hFile, SafeU64ToHex(hexBuf, sizeof(hexBuf), reinterpret_cast<unsigned __int64>(exc->ExceptionRecord->ExceptionAddress)));
        SafeWrite(hFile, "\n");

        SafeWrite(hFile, "Exception Flags: ");
        SafeWrite(hFile, SafeU64ToHex(hexBuf, sizeof(hexBuf), exc->ExceptionRecord->ExceptionFlags));
        SafeWrite(hFile, "\n");

        // 解释常见异常代码
        const char* exc_name = "Unknown";
        DWORD code = exc->ExceptionRecord->ExceptionCode;

        if (code == EXCEPTION_ACCESS_VIOLATION) {
            exc_name = "ACCESS_VIOLATION";
        } else if (code == EXCEPTION_STACK_OVERFLOW) {
            exc_name = "STACK_OVERFLOW";
        } else if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
            exc_name = "INT_DIVIDE_BY_ZERO";
        } else if (code == EXCEPTION_PRIV_INSTRUCTION) {
            exc_name = "PRIV_INSTRUCTION";
        } else if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {
            exc_name = "ILLEGAL_INSTRUCTION";
        }

        SafeWrite(hFile, "Exception Name: ");
        SafeWrite(hFile, exc_name);
        SafeWrite(hFile, "\n");

        // ACCESS_VIOLATION 额外信息
        if (code == EXCEPTION_ACCESS_VIOLATION && exc->ExceptionRecord->NumberParameters >= 2) {
            ULONG_PTR* params = exc->ExceptionRecord->ExceptionInformation;
            SafeWrite(hFile, "  Access Type: ");
            if (params[0] == 0) SafeWrite(hFile, "Read");
            else if (params[0] == 1) SafeWrite(hFile, "Write");
            else if (params[0] == 8) SafeWrite(hFile, "Execute");
            else SafeWrite(hFile, "Unknown");
            SafeWrite(hFile, "\n  Access Address: ");
            SafeWrite(hFile, SafeU64ToHex(hexBuf, sizeof(hexBuf), params[1]));
            SafeWrite(hFile, "\n");
        }

        // 寄存器状态 (x64)
#if defined(_M_X64)
        SafeWrite(hFile, "\n--- Registers (x64) ---\n");

        auto writeReg = [&](const char* name, unsigned __int64 val) {
            SafeWrite(hFile, name);
            SafeWrite(hFile, ": ");
            SafeWrite(hFile, SafeU64ToHex(hexBuf, sizeof(hexBuf), val));
            SafeWrite(hFile, "\n");
        };

        writeReg("RAX", exc->ContextRecord->Rax);
        writeReg("RBX", exc->ContextRecord->Rbx);
        writeReg("RCX", exc->ContextRecord->Rcx);
        writeReg("RDX", exc->ContextRecord->Rdx);
        writeReg("RSI", exc->ContextRecord->Rsi);
        writeReg("RDI", exc->ContextRecord->Rdi);
        writeReg("RBP", exc->ContextRecord->Rbp);
        writeReg("RSP", exc->ContextRecord->Rsp);
        writeReg("R8 ", exc->ContextRecord->R8);
        writeReg("R9 ", exc->ContextRecord->R9);
        writeReg("R10", exc->ContextRecord->R10);
        writeReg("R11", exc->ContextRecord->R11);
        writeReg("R12", exc->ContextRecord->R12);
        writeReg("R13", exc->ContextRecord->R13);
        writeReg("R14", exc->ContextRecord->R14);
        writeReg("R15", exc->ContextRecord->R15);
        writeReg("RIP", exc->ContextRecord->Rip);
#endif

        SafeWrite(hFile, "\n--- Stack Trace ---\n");
        SafeWrite(hFile, "(Stack trace disabled in async-safe mode)\n");

        CloseHandle(hFile);

        // 输出到 Debug 输出
        OutputDebugStringA("\n*** CRASH: ");
        OutputDebugStringA(exc_name);
        OutputDebugStringA(" ***\nSee ");
        OutputDebugStringA(crash_file);
        OutputDebugStringA(" for details\n");
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

inline void InstallCrashHandler() {
    SetUnhandledExceptionFilter(CrashHandler);
}

} // namespace voidview

#else
// Non-Windows: no-op
namespace voidview {
inline void InstallCrashHandler() {}
}
#endif

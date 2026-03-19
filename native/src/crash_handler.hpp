#pragma once

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <ctime>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace voidview {

inline LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exc) {
    // 获取当前时间
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y%m%d_%H%M%S", t);

    // 生成崩溃日志文件名
    std::string crash_file = "crash_" + std::string(time_buf) + ".log";

    FILE* f = fopen(crash_file.c_str(), "w");
    if (f) {
        fprintf(f, "=== VoidView Native Crash Report ===\n");
        fprintf(f, "Time: %s\n", time_buf);
        fprintf(f, "Exception Code: 0x%08X\n", exc->ExceptionRecord->ExceptionCode);
        fprintf(f, "Exception Address: 0x%p\n", exc->ExceptionRecord->ExceptionAddress);
        fprintf(f, "Exception Flags: 0x%08X\n", exc->ExceptionRecord->ExceptionFlags);

        // 解释常见异常代码
        const char* exc_name = "Unknown";
        switch (exc->ExceptionRecord->ExceptionCode) {
            case EXCEPTION_ACCESS_VIOLATION:
                exc_name = "ACCESS_VIOLATION";
                // 尝试读取违规地址
                if (exc->ExceptionRecord->NumberParameters >= 2) {
                    ULONG_PTR* params = exc->ExceptionRecord->ExceptionInformation;
                    fprintf(f, "  Access Type: %s\n",
                        params[0] == 0 ? "Read" :
                        params[0] == 1 ? "Write" :
                        params[0] == 8 ? "Execute" : "Unknown");
                    fprintf(f, "  Access Address: 0x%p\n", (void*)params[1]);
                }
                break;
            case EXCEPTION_STACK_OVERFLOW: exc_name = "STACK_OVERFLOW"; break;
            case EXCEPTION_INT_DIVIDE_BY_ZERO: exc_name = "INT_DIVIDE_BY_ZERO"; break;
            case EXCEPTION_PRIV_INSTRUCTION: exc_name = "PRIV_INSTRUCTION"; break;
            case EXCEPTION_ILLEGAL_INSTRUCTION: exc_name = "ILLEGAL_INSTRUCTION"; break;
        }
        fprintf(f, "Exception Name: %s\n", exc_name);

        // 寄存器状态 (x64)
#if defined(_M_X64)
        fprintf(f, "\n--- Registers (x64) ---\n");
        fprintf(f, "RAX: 0x%016llX  RBX: 0x%016llX\n", exc->ContextRecord->Rax, exc->ContextRecord->Rbx);
        fprintf(f, "RCX: 0x%016llX  RDX: 0x%016llX\n", exc->ContextRecord->Rcx, exc->ContextRecord->Rdx);
        fprintf(f, "RSI: 0x%016llX  RDI: 0x%016llX\n", exc->ContextRecord->Rsi, exc->ContextRecord->Rdi);
        fprintf(f, "RBP: 0x%016llX  RSP: 0x%016llX\n", exc->ContextRecord->Rbp, exc->ContextRecord->Rsp);
        fprintf(f, "R8:  0x%016llX  R9:  0x%016llX\n", exc->ContextRecord->R8, exc->ContextRecord->R9);
        fprintf(f, "R10: 0x%016llX  R11: 0x%016llX\n", exc->ContextRecord->R10, exc->ContextRecord->R11);
        fprintf(f, "R12: 0x%016llX  R13: 0x%016llX\n", exc->ContextRecord->R12, exc->ContextRecord->R13);
        fprintf(f, "R14: 0x%016llX  R15: 0x%016llX\n", exc->ContextRecord->R14, exc->ContextRecord->R15);
        fprintf(f, "RIP: 0x%016llX\n", exc->ContextRecord->Rip);
#endif

        // 堆栈跟踪
        fprintf(f, "\n--- Stack Trace ---\n");

        HANDLE process = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();

        // 初始化 DbgHelp
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        if (SymInitialize(process, nullptr, TRUE)) {
            // 捕获堆栈
            PVOID stack[64];
            USHORT frames = CaptureStackBackTrace(
                2,  // 跳过本函数
                64,
                stack,
                nullptr
            );

            SYMBOL_INFO* symbol = (SYMBOL_INFO*)malloc(sizeof(SYMBOL_INFO) + 256);
            if (symbol) {
                symbol->MaxNameLen = 255;
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

                for (USHORT i = 0; i < frames; i++) {
                    DWORD64 address = (DWORD64)stack[i];

                    fprintf(f, "[%2d] 0x%016llX ", i, address);

                    if (SymFromAddr(process, address, 0, symbol)) {
                        fprintf(f, "%s\n", symbol->Name);
                    } else {
                        fprintf(f, "(unknown)\n");
                    }
                }
                free(symbol);
            }

            SymCleanup(process);
        } else {
            fprintf(f, "(SymInitialize failed, no stack trace)\n");
        }

        fclose(f);

        // 也输出到 stderr（如果控制台可见）
        fprintf(stderr, "\n*** CRASH: %s at 0x%p ***\n", exc_name, exc->ExceptionRecord->ExceptionAddress);
        fprintf(stderr, "*** See %s for details ***\n", crash_file.c_str());
        fflush(stderr);
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

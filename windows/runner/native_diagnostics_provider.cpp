#include "native_diagnostics_provider.h"

#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>
#include <chrono>
#include <mutex>
#include <vector>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")

ProcessMemoryUsage NativeDiagnosticsProvider::QueryProcessMemoryUsage() const {
    ProcessMemoryUsage usage;
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        usage.working_set_bytes = static_cast<uint64_t>(counters.WorkingSetSize);
        usage.private_bytes = static_cast<uint64_t>(counters.PrivateUsage);
    }
    return usage;
}

ProcessHeapUsage NativeDiagnosticsProvider::QueryProcessHeapUsage() const {
    ProcessHeapUsage usage;
    DWORD count = GetProcessHeaps(0, nullptr);
    if (count == 0) {
        return usage;
    }

    std::vector<HANDLE> heaps(count);
    DWORD written = GetProcessHeaps(count, heaps.data());
    if (written == 0) {
        return usage;
    }
    if (written > count) {
        heaps.resize(written);
        written = GetProcessHeaps(written, heaps.data());
        if (written == 0) {
            return usage;
        }
    }

    usage.heap_count = written;
    for (DWORD i = 0; i < written; ++i) {
        HEAP_SUMMARY summary = {};
        summary.cb = sizeof(summary);
        if (!HeapSummary(heaps[i], 0, &summary)) {
            continue;
        }
        usage.allocated_bytes += static_cast<uint64_t>(summary.cbAllocated);
        usage.committed_bytes += static_cast<uint64_t>(summary.cbCommitted);
        usage.reserved_bytes += static_cast<uint64_t>(summary.cbReserved);
    }
    return usage;
}

uint64_t NativeDiagnosticsProvider::QueryDedicatedVideoMemoryUsage() const {
    using Clock = std::chrono::steady_clock;
    static std::mutex cache_mutex;
    static Clock::time_point last_query{};
    static uint64_t cached_usage = 0;
    static constexpr auto kCacheTtl = std::chrono::seconds(2);

    const auto now = Clock::now();
    {
        std::lock_guard lock(cache_mutex);
        if (last_query != Clock::time_point{} && now - last_query < kCacheTtl) {
            return cached_usage;
        }
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        std::lock_guard lock(cache_mutex);
        last_query = now;
        cached_usage = 0;
        return cached_usage;
    }

    uint64_t total_usage = 0;
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr) || !adapter) {
            continue;
        }

        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        if (FAILED(adapter.As(&adapter3)) || !adapter3) {
            continue;
        }

        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            total_usage += static_cast<uint64_t>(info.CurrentUsage);
        }
    }
    std::lock_guard lock(cache_mutex);
    last_query = now;
    cached_usage = total_usage;
    return cached_usage;
}

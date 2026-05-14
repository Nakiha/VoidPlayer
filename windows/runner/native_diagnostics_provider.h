#pragma once

#include <cstdint>

struct ProcessMemoryUsage {
    uint64_t working_set_bytes = 0;
    uint64_t private_bytes = 0;
};

struct ProcessHeapUsage {
    uint64_t allocated_bytes = 0;
    uint64_t committed_bytes = 0;
    uint64_t reserved_bytes = 0;
    uint32_t heap_count = 0;
};

class NativeDiagnosticsProvider {
public:
    ProcessMemoryUsage QueryProcessMemoryUsage() const;
    ProcessHeapUsage QueryProcessHeapUsage() const;
    uint64_t QueryDedicatedVideoMemoryUsage() const;
};

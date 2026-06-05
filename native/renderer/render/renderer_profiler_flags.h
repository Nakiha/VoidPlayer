#pragma once

#include <cstdlib>
#include <cstring>

namespace vr {

inline bool profiler_enabled(const char* env_name) {
    const char* value = std::getenv(env_name);
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0;
}

} // namespace vr

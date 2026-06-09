#pragma once

#include <string>
#include <utility>

namespace vr {

enum class NativeErrorCode {
    Ok = 0,
    InvalidArgument,
    NotInitialized,
    AlreadyRunning,
    OpenFailed,
    BackendUnavailable,
    DecoderUnavailable,
    Cancelled,
    Internal,
};

template <typename T>
struct NativeResult {
    bool ok = true;
    NativeErrorCode code = NativeErrorCode::Ok;
    std::string message;
    T value{};

    explicit operator bool() const { return ok; }
};

template <>
struct NativeResult<void> {
    bool ok = true;
    NativeErrorCode code = NativeErrorCode::Ok;
    std::string message;

    explicit operator bool() const { return ok; }
};

inline NativeResult<void> native_ok() {
    return {};
}

template <typename T>
NativeResult<T> native_ok(T value) {
    NativeResult<T> result;
    result.value = std::move(value);
    return result;
}

template <typename T>
NativeResult<T> native_error(NativeErrorCode code, std::string message) {
    NativeResult<T> result;
    result.ok = false;
    result.code = code;
    result.message = std::move(message);
    return result;
}

inline NativeResult<void> native_error(NativeErrorCode code, std::string message) {
    NativeResult<void> result;
    result.ok = false;
    result.code = code;
    result.message = std::move(message);
    return result;
}

} // namespace vr

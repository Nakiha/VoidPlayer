#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__unix__)
#include <unistd.h>
#endif

namespace vr::win_utf8 {

#ifdef _WIN32

struct Utf16FromUtf8Result {
    bool ok = false;
    DWORD error = 0;
    std::wstring value;
};

struct Utf8FromUtf16Result {
    bool ok = false;
    DWORD error = 0;
    std::string value;
};

inline Utf16FromUtf8Result try_utf16_from_utf8(const std::string& utf8) {
    Utf16FromUtf8Result result;
    if (utf8.empty()) {
        result.ok = true;
        return result;
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, nullptr, 0);
    if (length <= 0) {
        result.error = GetLastError();
        return result;
    }
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, wide.data(), length) <= 0) {
        result.error = GetLastError();
        return result;
    }
    wide.resize(static_cast<size_t>(length - 1));
    result.ok = true;
    result.value = std::move(wide);
    return result;
}

inline std::wstring utf16_from_utf8(const std::string& utf8) {
    auto result = try_utf16_from_utf8(utf8);
    return result.ok ? std::move(result.value) : std::wstring{};
}

inline bool is_valid_utf8(const std::string& utf8) {
    return try_utf16_from_utf8(utf8).ok;
}

inline Utf8FromUtf16Result try_utf8_from_utf16(const wchar_t* utf16) {
    Utf8FromUtf16Result result;
    if (!utf16 || utf16[0] == L'\0') {
        result.ok = true;
        return result;
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, utf16, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        result.error = GetLastError();
        return result;
    }
    std::string utf8(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, utf16, -1, utf8.data(), length, nullptr, nullptr) <= 0) {
        result.error = GetLastError();
        return result;
    }
    utf8.resize(static_cast<size_t>(length - 1));
    result.ok = true;
    result.value = std::move(utf8);
    return result;
}

inline std::string utf8_from_utf16(const wchar_t* utf16) {
    auto result = try_utf8_from_utf16(utf16);
    return result.ok ? std::move(result.value) : std::string{};
}

inline std::filesystem::path path_from_utf8(const std::string& utf8) {
    return std::filesystem::path(utf16_from_utf8(utf8));
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
    return utf8_from_utf16(path.c_str());
}

inline std::string module_directory_utf8(HMODULE module = nullptr) {
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = 0;
    while (true) {
        length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) break;
        buffer.resize(buffer.size() * 2);
    }

    std::filesystem::path path(buffer.data());
    return path_to_utf8(path.parent_path());
}

inline bool create_directory_utf8(const std::string& path) {
    const auto wide = utf16_from_utf8(path);
    if (wide.empty()) return false;
    if (CreateDirectoryW(wide.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

inline bool file_exists_utf8(const std::string& path) {
    const auto wide = utf16_from_utf8(path);
    if (wide.empty()) return false;
    return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline bool delete_file_utf8(const std::string& path) {
    const auto wide = utf16_from_utf8(path);
    if (wide.empty()) return false;
    if (DeleteFileW(wide.c_str())) return true;
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

inline std::string get_env_utf8(const wchar_t* name) {
    if (!name || name[0] == L'\0') return {};
    DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) return {};
    std::wstring value(length, L'\0');
    length = GetEnvironmentVariableW(name, value.data(), length);
    if (length == 0) return {};
    value.resize(length);
    return utf8_from_utf16(value.c_str());
}

inline bool set_env_utf8(const wchar_t* name, const std::string& value) {
    if (!name || name[0] == L'\0') return false;
    if (value.empty()) {
        return SetEnvironmentVariableW(name, nullptr) != FALSE;
    }
    const auto wide_value = utf16_from_utf8(value);
    if (wide_value.empty()) return false;
    return SetEnvironmentVariableW(name, wide_value.c_str()) != FALSE;
}

#else

inline bool is_valid_utf8(const std::string&) {
    return true;
}

inline std::filesystem::path path_from_utf8(const std::string& utf8) {
    return std::filesystem::u8path(utf8);
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
    return path.u8string();
}

inline std::string module_directory_utf8() {
#if defined(__APPLE__)
    uint32_t length = 1024;
    std::vector<char> buffer(static_cast<size_t>(length), '\0');
    if (_NSGetExecutablePath(buffer.data(), &length) != 0) {
        buffer.assign(static_cast<size_t>(length) + 1, '\0');
        if (_NSGetExecutablePath(buffer.data(), &length) != 0) return {};
    }
    std::error_code ec;
    auto path = std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), ec);
    if (ec) path = std::filesystem::path(buffer.data());
    return path_to_utf8(path.parent_path());
#elif defined(__unix__)
    std::vector<char> buffer(4096, '\0');
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return {};
    buffer[static_cast<size_t>(length)] = '\0';
    return path_to_utf8(std::filesystem::path(buffer.data()).parent_path());
#else
    std::error_code ec;
    auto path = std::filesystem::current_path(ec);
    return ec ? std::string{} : path_to_utf8(path);
#endif
}

inline bool create_directory_utf8(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path_from_utf8(path), ec);
    return !ec;
}

inline bool file_exists_utf8(const std::string& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path_from_utf8(path), ec) && !ec;
}

inline bool delete_file_utf8(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path_from_utf8(path), ec);
    return !ec;
}

inline std::string narrow_env_name(const wchar_t* name) {
    std::string result;
    if (!name) return result;
    for (const wchar_t* it = name; *it != L'\0'; ++it) {
        if (*it < 0 || *it > 0x7f) return {};
        result.push_back(static_cast<char>(*it));
    }
    return result;
}

inline std::string get_env_utf8(const wchar_t* name) {
    const auto narrow = narrow_env_name(name);
    if (narrow.empty()) return {};
    const char* value = std::getenv(narrow.c_str());
    return value ? std::string(value) : std::string{};
}

inline bool set_env_utf8(const wchar_t* name, const std::string& value) {
    const auto narrow = narrow_env_name(name);
    if (narrow.empty()) return false;
    if (value.empty()) {
        return unsetenv(narrow.c_str()) == 0;
    }
    return setenv(narrow.c_str(), value.c_str(), 1) == 0;
}

#endif

} // namespace vr::win_utf8

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vr::win_utf8 {

#ifdef _WIN32

inline std::wstring utf16_from_utf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), length) <= 0) {
        return {};
    }
    wide.resize(static_cast<size_t>(length - 1));
    return wide;
}

inline std::string utf8_from_utf16(const wchar_t* utf16) {
    if (!utf16 || utf16[0] == L'\0') return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, utf16, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, utf16, -1, utf8.data(), length, nullptr, nullptr) <= 0) {
        return {};
    }
    utf8.resize(static_cast<size_t>(length - 1));
    return utf8;
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

inline std::filesystem::path path_from_utf8(const std::string& utf8) {
    return std::filesystem::u8path(utf8);
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
    return path.u8string();
}

#endif

} // namespace vr::win_utf8

#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace vp_tools {

inline bool is_git_lfs_pointer(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    char prefix[64] = {};
    input.read(prefix, sizeof(prefix));
    const std::string text(prefix, static_cast<size_t>(input.gcount()));
    return text.rfind("version https://git-lfs.github.com/spec/v1", 0) == 0;
}

inline std::string h264_smoke_video_path(const std::string& root) {
    if (root.empty()) {
        return {};
    }
    const std::filesystem::path root_path(root);
    const auto preferred = root_path / "h264_9s_1920x1080.mp4";
    if (std::filesystem::is_regular_file(preferred) && !is_git_lfs_pointer(preferred)) {
        return preferred.string();
    }
    const auto fallback = root_path / "ci_h264_smoke.mp4";
    if (std::filesystem::is_regular_file(fallback) && !is_git_lfs_pointer(fallback)) {
        return fallback.string();
    }
    return {};
}

}  // namespace vp_tools

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/fmt/fmt.h>

#include <memory>
#include <string>
#include <vector>

namespace voidview {

// 日志格式常量
// 与 Python loguru 格式对齐: time | level | source - message
// native 模块使用 [native] 作为 source 标识
inline constexpr const char* CONSOLE_PATTERN =
    "%^%Y-%m-%d %H:%M:%S|%8l|[native] - %v%$";  // 带颜色
inline constexpr const char* FILE_PATTERN =
    "%Y-%m-%d %H:%M:%S|%8l|[native] - %v";       // 不带颜色

// 默认轮转配置 (与 Python loguru 对齐)
inline constexpr size_t DEFAULT_MAX_FILE_SIZE = 10 * 1024 * 1024;  // 10 MB
inline constexpr size_t DEFAULT_MAX_FILES = 3;  // 保留 3 个轮转文件

// 全局日志器和 sink 列表
inline std::shared_ptr<spdlog::logger>& get_logger() {
    static std::shared_ptr<spdlog::logger> logger = [] {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_pattern(CONSOLE_PATTERN);
        auto lg = std::make_shared<spdlog::logger>("voidview", console);
        lg->set_level(spdlog::level::info);  // 默认 INFO 级别
        lg->flush_on(spdlog::level::warn);
        return lg;
    }();
    return logger;
}

// 日志级别转换 (Python -> spdlog)
inline spdlog::level::level_enum to_spdlog_level(int level) {
    switch (level) {
        case 0: return spdlog::level::trace;
        case 1: return spdlog::level::debug;
        case 2: return spdlog::level::info;
        case 3: return spdlog::level::warn;
        case 4: return spdlog::level::err;
        case 5: return spdlog::level::critical;
        case 6: return spdlog::level::off;
        default: return spdlog::level::info;
    }
}

// spdlog 级别转换 (spdlog -> Python)
inline int from_spdlog_level(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::trace: return 0;
        case spdlog::level::debug: return 1;
        case spdlog::level::info: return 2;
        case spdlog::level::warn: return 3;
        case spdlog::level::err: return 4;
        case spdlog::level::critical: return 5;
        case spdlog::level::off: return 6;
        default: return 2;
    }
}

// 设置日志级别 (供 Python 调用)
inline void set_log_level(int level) {
    get_logger()->set_level(to_spdlog_level(level));
}

// 获取当前日志级别
inline int get_log_level() {
    return from_spdlog_level(get_logger()->level());
}

// 添加文件落盘 sink (供 Python 调用)
// log_path: 日志文件完整路径
// max_size: 最大文件大小 (字节)，默认 10MB
// max_files: 最大文件数量，默认 3
inline void add_file_sink(
    const std::string& log_path,
    size_t max_size = DEFAULT_MAX_FILE_SIZE,
    size_t max_files = DEFAULT_MAX_FILES
) {
    try {
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_path, max_size, max_files
        );
        file_sink->set_pattern(FILE_PATTERN);
        file_sink->set_level(spdlog::level::trace);  // 文件记录所有级别
        get_logger()->sinks().push_back(file_sink);
        get_logger()->info(
            "Native 日志配置: path={}, max_size={}MB, max_files={}",
            log_path, max_size / (1024 * 1024), max_files
        );
    } catch (const spdlog::spdlog_ex& ex) {
        get_logger()->error("Failed to add file sink: {}", ex.what());
    }
}

// 便捷宏
#define VV_TRACE(...)    voidview::get_logger()->trace(__VA_ARGS__)
#define VV_DEBUG(...)    voidview::get_logger()->debug(__VA_ARGS__)
#define VV_INFO(...)     voidview::get_logger()->info(__VA_ARGS__)
#define VV_WARN(...)     voidview::get_logger()->warn(__VA_ARGS__)
#define VV_ERROR(...)    voidview::get_logger()->error(__VA_ARGS__)
#define VV_CRITICAL(...) voidview::get_logger()->critical(__VA_ARGS__)

} // namespace voidview

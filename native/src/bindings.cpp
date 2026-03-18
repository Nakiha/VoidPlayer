#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "voidview_native/hardware_decoder.hpp"
#include "voidview_native/media_probe.hpp"
#include "voidview_native/logger.hpp"
#include "voidview_native/version.hpp"

extern "C" {
#include <libavutil/log.h>
}

namespace py = pybind11;

// FFmpeg 日志回调 - 转发到 spdlog
static void ffmpeg_log_callback(void* ptr, int level, const char* fmt, va_list vl) {
    if (level > av_log_get_level()) return;

    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, vl);

    // 去除末尾的换行符
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
        buf[--len] = '\0';
    }

    // 映射 FFmpeg 日志级别到 spdlog
    switch (level) {
        case AV_LOG_QUIET:
            break;
        case AV_LOG_PANIC:
        case AV_LOG_FATAL:
        case AV_LOG_ERROR:
            VV_ERROR("[FFmpeg] {}", buf);
            break;
        case AV_LOG_WARNING:
            VV_WARN("[FFmpeg] {}", buf);
            break;
        case AV_LOG_INFO:
            VV_INFO("[FFmpeg] {}", buf);
            break;
        case AV_LOG_VERBOSE:
        case AV_LOG_DEBUG:
            VV_DEBUG("[FFmpeg] {}", buf);
            break;
        case AV_LOG_TRACE:
            VV_TRACE("[FFmpeg] {}", buf);
            break;
        default:
            VV_DEBUG("[FFmpeg] {}", buf);
            break;
    }
}

// 初始化日志系统 (Python 端调用)
static void init_logging(int level) {
    // 设置 voidview 日志级别
    voidview::set_log_level(level);

    // 设置 FFmpeg 日志回调
    av_log_set_callback(ffmpeg_log_callback);

    // 映射 Python 日志级别到 FFmpeg
    int ffmpeg_level = AV_LOG_INFO;
    switch (level) {
        case 0: ffmpeg_level = AV_LOG_TRACE; break;    // TRACE
        case 1: ffmpeg_level = AV_LOG_DEBUG; break;    // DEBUG
        case 2: ffmpeg_level = AV_LOG_INFO; break;     // INFO
        case 3: ffmpeg_level = AV_LOG_WARNING; break;  // WARN
        case 4: ffmpeg_level = AV_LOG_ERROR; break;    // ERROR
        case 5: ffmpeg_level = AV_LOG_FATAL; break;    // CRITICAL
        case 6: ffmpeg_level = AV_LOG_QUIET; break;    // OFF
    }
    av_log_set_level(ffmpeg_level);
}

PYBIND11_MODULE(voidview_native, m) {
    m.doc() = "VoidView Native Module - Hardware Accelerated Video Decoder";

    // 版本信息
    m.attr("__version__") = VOIDVIEW_VERSION_STRING;
    m.attr("__build_time__") = VOIDVIEW_BUILD_TIME;

    // Hardware type constants
    m.attr("HW_AUTO") = 0;
    m.attr("HW_D3D11VA") = 1;
    m.attr("HW_NVDEC") = 2;

    // Log level constants (与 Python logging 对齐)
    m.attr("LOG_TRACE") = 0;
    m.attr("LOG_DEBUG") = 1;
    m.attr("LOG_INFO") = 2;
    m.attr("LOG_WARN") = 3;
    m.attr("LOG_ERROR") = 4;
    m.attr("LOG_CRITICAL") = 5;
    m.attr("LOG_OFF") = 6;

    // 日志控制函数
    m.def("set_log_level", &voidview::set_log_level,
          py::arg("level"),
          "Set native log level (0=TRACE, 1=DEBUG, 2=INFO, 3=WARN, 4=ERROR, 5=CRITICAL, 6=OFF)");

    m.def("get_log_level", &voidview::get_log_level,
          "Get current native log level");

    m.def("init_logging", &init_logging,
          py::arg("level") = 2,
          "Initialize logging system with specified level (also sets FFmpeg log callback)");

    m.def("add_file_sink", &voidview::add_file_sink,
          py::arg("log_path"),
          py::arg("max_size") = 10 * 1024 * 1024,
          py::arg("max_files") = 3,
          "Add file sink for native log output with rotation");

    // MediaInfo class
    py::class_<voidview::MediaInfo>(m, "MediaInfo")
        .def_readonly("valid", &voidview::MediaInfo::valid)
        .def_readonly("error_message", &voidview::MediaInfo::error_message)
        .def_readonly("width", &voidview::MediaInfo::width)
        .def_readonly("height", &voidview::MediaInfo::height)
        .def_readonly("duration_ms", &voidview::MediaInfo::duration_ms)
        .def_readonly("fps", &voidview::MediaInfo::fps)
        .def_readonly("codec_name", &voidview::MediaInfo::codec_name)
        .def_readonly("pixel_format", &voidview::MediaInfo::pixel_format)
        .def_readonly("format_name", &voidview::MediaInfo::format_name)
        .def_readonly("format_long_name", &voidview::MediaInfo::format_long_name)
        .def_readonly("bit_rate", &voidview::MediaInfo::bit_rate)
        .def_readonly("seekable", &voidview::MediaInfo::seekable)
        .def_readonly("has_audio", &voidview::MediaInfo::has_audio)
        .def("__repr__", [](const voidview::MediaInfo& info) {
            return "<MediaInfo valid=" + std::string(info.valid ? "True" : "False") +
                   " width=" + std::to_string(info.width) +
                   " height=" + std::to_string(info.height) +
                   " duration_ms=" + std::to_string(info.duration_ms) +
                   " fps=" + std::to_string(info.fps) + ">";
        });

    // probe_file function
    m.def("probe_file", &voidview::probe_file,
          py::arg("url"),
          "Probe media file for information (lightweight, no decoder initialization)");

    // HardwareDecoder class
    py::class_<voidview::HardwareDecoder>(m, "HardwareDecoder")
        .def(py::init<const std::string&>(), py::arg("source_url"),
             "Create a decoder for the given video source")

        .def("initialize", &voidview::HardwareDecoder::initialize,
             py::arg("hw_device_type") = 0,
             "Initialize decoder. hw_device_type: 0=Auto, 1=D3D11VA, 2=NVDEC")

        .def("set_opengl_context", [](voidview::HardwareDecoder& self, uintptr_t gl_context) {
            self.set_opengl_context(reinterpret_cast<void*>(gl_context));
        }, py::arg("gl_context"),
           "Set OpenGL context for texture sharing (pass 0 for current context)")

        .def("decode_next_frame", &voidview::HardwareDecoder::decode_next_frame,
             "Decode next frame. Returns True if new frame available")

        .def("seek_to", &voidview::HardwareDecoder::seek_to,
             py::arg("timestamp_ms"),
             "Seek to timestamp in milliseconds (keyframe-level, fast)")

        .def("seek_to_precise", &voidview::HardwareDecoder::seek_to_precise,
             py::arg("timestamp_ms"),
             "Seek to exact frame before timestamp (frame-accurate, slower)")

        .def_property_readonly("texture_id", &voidview::HardwareDecoder::get_texture_id,
             "OpenGL texture ID of current frame")

        .def_property_readonly("current_pts_ms", &voidview::HardwareDecoder::get_current_pts_ms,
             "Current frame timestamp in milliseconds")

        .def_property_readonly("duration_ms", &voidview::HardwareDecoder::get_duration_ms,
             "Total duration in milliseconds")

        .def_property_readonly("width", &voidview::HardwareDecoder::get_width,
             "Video width in pixels")

        .def_property_readonly("height", &voidview::HardwareDecoder::get_height,
             "Video height in pixels")

        .def_property_readonly("seekable", &voidview::HardwareDecoder::is_seekable,
             "Whether the source is seekable")

        .def_property_readonly("eof", &voidview::HardwareDecoder::is_eof,
             "Whether end of file reached")

        .def_property_readonly("has_error", &voidview::HardwareDecoder::has_error,
             "Whether an error occurred")

        .def_property_readonly("error_message", &voidview::HardwareDecoder::get_error_message,
             "Last error message if any");
}

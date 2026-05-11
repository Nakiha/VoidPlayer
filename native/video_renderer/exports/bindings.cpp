#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "player/native_player.h"
#include "video_renderer/layout_validation.h"
#include "video_renderer/renderer_config_validation.h"
#include "video_renderer/renderer.h"
#include "common/logging.h"
#include "common/windows_crash_handler.h"
#include <cstdint>

namespace py = pybind11;

namespace {

void validate_layout_or_throw(const vr::LayoutState& state) {
    const auto result = vr::validate_layout_state(state);
    if (!result.ok) {
        throw py::value_error(result.message);
    }
}

void validate_config_or_throw(const vr::RendererConfig& config) {
    const auto result = vr::validate_renderer_config(config);
    if (!result.ok) {
        throw py::value_error(result.message);
    }
}

void validate_speed_or_throw(double speed) {
    const auto result = vr::validate_playback_speed(speed);
    if (!result.ok) {
        throw py::value_error(result.message);
    }
}

void validate_loop_range_or_throw(bool enabled, int64_t start_us, int64_t end_us) {
    const auto result = vr::validate_loop_range(enabled, start_us, end_us);
    if (!result.ok) {
        throw py::value_error(result.message);
    }
}

} // namespace

PYBIND11_MODULE(video_renderer_native, m) {
    m.doc() = "Video Renderer Native Module - D3D11VA Multi-track Video Renderer";

    // SeekType enum
    py::enum_<vr::SeekType>(m, "SeekType")
        .value("Keyframe", vr::SeekType::Keyframe)
        .value("Exact", vr::SeekType::Exact);

    // LogConfig
    py::class_<vr::LogConfig>(m, "LogConfig")
        .def(py::init<>())
        .def_readwrite("pattern", &vr::LogConfig::pattern)
        .def_readwrite("file_path", &vr::LogConfig::file_path)
        .def_readwrite("max_file_size", &vr::LogConfig::max_file_size)
        .def_readwrite("max_files", &vr::LogConfig::max_files)
        .def_readwrite("level", &vr::LogConfig::level)
        .def_readwrite("configure_console_codepage", &vr::LogConfig::configure_console_codepage)
        .def_readwrite("use_environment_level_override", &vr::LogConfig::use_environment_level_override)
        .def_readwrite("manage_global_flush", &vr::LogConfig::manage_global_flush);

    // LayoutState — atomic layout parameter block
    py::class_<vr::LayoutState>(m, "LayoutState")
        .def(py::init<>())
        .def_readwrite("mode", &vr::LayoutState::mode,
            "Layout mode: 0=SIDE_BY_SIDE, 1=SPLIT_SCREEN")
        .def_readwrite("split_pos", &vr::LayoutState::split_pos,
            "Split divider position (0.0-1.0)")
        .def_readwrite("zoom_ratio", &vr::LayoutState::zoom_ratio,
            "Zoom ratio (1.0=fit, >1.0=zoom in)")
        .def_readwrite("pixel_size_mode", &vr::LayoutState::pixel_size_mode,
            "Pixel size mode: 0=UNIFORM_VIDEO_PIXELS, 1=FILL_VIEW")
        // view_offset: expose as Python list [x, y]
        .def_property("view_offset",
            [](vr::LayoutState& s) -> std::vector<float> {
                return {s.view_offset[0], s.view_offset[1]};
            },
            [](vr::LayoutState& s, const std::vector<float>& v) {
                if (v.size() != 2) {
                    throw py::value_error("view_offset must contain exactly 2 values");
                }
                s.view_offset[0] = v[0];
                s.view_offset[1] = v[1];
            },
            "Pan offset [x, y] in pixel coordinates")
        // order: expose as Python list [0, 1, 2, 3]
        .def_property("order",
            [](vr::LayoutState& s) -> std::vector<int> {
                return {s.order[0], s.order[1], s.order[2], s.order[3]};
            },
            [](vr::LayoutState& s, const std::vector<int>& v) {
                if (v.size() != 4) {
                    throw py::value_error("order must contain exactly 4 file IDs");
                }
                for (size_t i = 0; i < 4; ++i) {
                    s.order[i] = v[i];
                }
            },
            "Track display order as list of file IDs");

    // RendererConfig
    py::class_<vr::RendererConfig>(m, "RendererConfig")
        .def(py::init<>())
        .def_readwrite("video_paths", &vr::RendererConfig::video_paths)
        .def_property("hwnd",
            [](vr::RendererConfig& c) -> int64_t { return reinterpret_cast<int64_t>(c.hwnd); },
            [](vr::RendererConfig& c, int64_t v) { c.hwnd = reinterpret_cast<void*>(v); })
        .def_readwrite("width", &vr::RendererConfig::width)
        .def_readwrite("height", &vr::RendererConfig::height)
        .def_readwrite("use_hardware_decode", &vr::RendererConfig::use_hardware_decode)
        .def_readwrite("log_config", &vr::RendererConfig::log_config);

    // Renderer
    py::class_<vr::Renderer>(m, "Renderer")
        .def(py::init<>())
        .def("initialize", [](vr::Renderer& r, const vr::RendererConfig& config) {
            validate_config_or_throw(config);
            py::gil_scoped_release release;
            return r.initialize(config);
        }, py::arg("config"))
        .def("shutdown", [](vr::Renderer& r) {
            py::gil_scoped_release release;
            r.shutdown();
        })
        .def("play", &vr::Renderer::play)
        .def("pause", &vr::Renderer::pause)
        .def("seek", [](vr::Renderer& r, int64_t target_pts_us, vr::SeekType type) {
            py::gil_scoped_release release;
            r.seek(target_pts_us, type);
        }, py::arg("target_pts_us"), py::arg("type") = vr::SeekType::Keyframe)
        .def("set_speed", [](vr::Renderer& r, double speed) {
            validate_speed_or_throw(speed);
            py::gil_scoped_release release;
            r.set_speed(speed);
        }, py::arg("speed"))
        .def("set_loop_range", [](vr::Renderer& r,
                                  bool enabled,
                                  int64_t start_us,
                                  int64_t end_us) {
            validate_loop_range_or_throw(enabled, start_us, end_us);
            py::gil_scoped_release release;
            r.set_loop_range(enabled, start_us, end_us);
        },
             py::arg("enabled"), py::arg("start_us"), py::arg("end_us"))
        .def("set_audible_track", &vr::Renderer::set_audible_track, py::arg("file_id"))
        .def("step_forward", &vr::Renderer::step_forward)
        .def("step_backward", &vr::Renderer::step_backward)
        .def("is_playing", &vr::Renderer::is_playing)
        .def("is_initialized", &vr::Renderer::is_initialized)
        .def("current_pts_us", &vr::Renderer::current_pts_us)
        .def("current_speed", &vr::Renderer::current_speed)
        .def("track_count", &vr::Renderer::track_count)
        .def("duration_us", &vr::Renderer::duration_us)
        // Dynamic track management
        .def("add_track", [](vr::Renderer& r,
                             const std::string& video_path,
                             bool use_hardware_decode) {
            py::gil_scoped_release release;
            return r.add_track(video_path, use_hardware_decode);
        }, py::arg("video_path"), py::arg("use_hardware_decode") = true,
             "Add a video track, returns slot index (0-3) or -1 on failure")
        .def("remove_track", [](vr::Renderer& r, int file_id) {
            py::gil_scoped_release release;
            r.remove_track(file_id);
        }, py::arg("file_id"),
             "Remove track by file_id")
        .def("has_track", &vr::Renderer::has_track, py::arg("slot"),
             "Check if slot has a track")
        .def("set_track_offset", [](vr::Renderer& r, int file_id, int64_t offset_us) {
            py::gil_scoped_release release;
            r.set_track_offset(file_id, offset_us);
        }, py::arg("file_id"), py::arg("offset_us"))
        // Layout control
        .def("apply_layout", [](vr::Renderer& r, const vr::LayoutState& state) {
            validate_layout_or_throw(state);
            py::gil_scoped_release release;
            r.apply_layout(state);
        }, py::arg("state"),
             "Atomically apply layout state and trigger redraw if paused")
        .def("layout", &vr::Renderer::layout,
             "Get a snapshot of the current layout state");

    // NativePlayer
    py::class_<vr::NativePlayer>(m, "NativePlayer")
        .def(py::init<>())
        .def("initialize", [](vr::NativePlayer& p, const vr::RendererConfig& config) {
            validate_config_or_throw(config);
            py::gil_scoped_release release;
            return p.initialize(config);
        }, py::arg("config"))
        .def("shutdown", [](vr::NativePlayer& p) {
            py::gil_scoped_release release;
            p.shutdown();
        })
        .def("play", &vr::NativePlayer::play)
        .def("pause", &vr::NativePlayer::pause)
        .def("seek", [](vr::NativePlayer& p, int64_t target_pts_us, vr::SeekType type) {
            py::gil_scoped_release release;
            p.seek(target_pts_us, type);
        }, py::arg("target_pts_us"), py::arg("type") = vr::SeekType::Keyframe)
        .def("set_speed", [](vr::NativePlayer& p, double speed) {
            validate_speed_or_throw(speed);
            py::gil_scoped_release release;
            p.set_speed(speed);
        }, py::arg("speed"))
        .def("set_loop_range", [](vr::NativePlayer& p,
                                  bool enabled,
                                  int64_t start_us,
                                  int64_t end_us) {
            validate_loop_range_or_throw(enabled, start_us, end_us);
            py::gil_scoped_release release;
            p.set_loop_range(enabled, start_us, end_us);
        },
             py::arg("enabled"), py::arg("start_us"), py::arg("end_us"))
        .def("set_audible_track", &vr::NativePlayer::set_audible_track, py::arg("file_id"))
        .def("step_forward", &vr::NativePlayer::step_forward)
        .def("step_backward", &vr::NativePlayer::step_backward)
        .def("is_playing", &vr::NativePlayer::is_playing)
        .def("is_initialized", &vr::NativePlayer::is_initialized)
        .def("current_pts_us", &vr::NativePlayer::current_pts_us)
        .def("current_speed", &vr::NativePlayer::current_speed)
        .def("track_count", &vr::NativePlayer::track_count)
        .def("duration_us", &vr::NativePlayer::duration_us)
        .def("add_track", [](vr::NativePlayer& p,
                             const std::string& video_path,
                             bool use_hardware_decode) {
            py::gil_scoped_release release;
            return p.add_track(video_path, use_hardware_decode);
        }, py::arg("video_path"), py::arg("use_hardware_decode") = true)
        .def("remove_track", [](vr::NativePlayer& p, int file_id) {
            py::gil_scoped_release release;
            p.remove_track(file_id);
        }, py::arg("file_id"))
        .def("has_track", &vr::NativePlayer::has_track, py::arg("slot"))
        .def("set_track_offset", [](vr::NativePlayer& p, int file_id, int64_t offset_us) {
            py::gil_scoped_release release;
            p.set_track_offset(file_id, offset_us);
        }, py::arg("file_id"), py::arg("offset_us"))
        .def("apply_layout", [](vr::NativePlayer& p, const vr::LayoutState& state) {
            validate_layout_or_throw(state);
            py::gil_scoped_release release;
            p.apply_layout(state);
        }, py::arg("state"))
        .def("layout", &vr::NativePlayer::layout);

    // Layout mode constants
    m.attr("LAYOUT_SIDE_BY_SIDE") = py::int_(vr::LAYOUT_SIDE_BY_SIDE);
    m.attr("LAYOUT_SPLIT_SCREEN") = py::int_(vr::LAYOUT_SPLIT_SCREEN);

    // Standalone logging functions
    m.def("configure_logging", &vr::configure_logging, py::arg("config"),
          "Configure spdlog with custom format, file path, and level");
    m.def("install_crash_handler", &vr::install_crash_handler, py::arg("crash_dir"),
          "Install Windows SEH crash handler that writes to crash_dir");
    m.def("remove_crash_handler", &vr::remove_crash_handler,
          "Remove the crash handler");
}

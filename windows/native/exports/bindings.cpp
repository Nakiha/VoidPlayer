#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "video_renderer/renderer.h"
#include "video_renderer/logging.h"
#include <cstdint>

namespace py = pybind11;

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
        .def_readwrite("level", &vr::LogConfig::level);

    // LayoutState — atomic layout parameter block
    py::class_<vr::LayoutState>(m, "LayoutState")
        .def(py::init<>())
        .def_readwrite("mode", &vr::LayoutState::mode,
            "Layout mode: 0=SIDE_BY_SIDE, 1=SPLIT_SCREEN")
        .def_readwrite("split_pos", &vr::LayoutState::split_pos,
            "Split divider position (0.0-1.0)")
        .def_readwrite("zoom_ratio", &vr::LayoutState::zoom_ratio,
            "Zoom ratio (1.0=fit, >1.0=zoom in)")
        // view_offset: expose as Python list [x, y]
        .def_property("view_offset",
            [](vr::LayoutState& s) -> std::vector<float> {
                return {s.view_offset[0], s.view_offset[1]};
            },
            [](vr::LayoutState& s, const std::vector<float>& v) {
                if (v.size() >= 2) {
                    s.view_offset[0] = v[0];
                    s.view_offset[1] = v[1];
                }
            },
            "Pan offset [x, y] in pixel coordinates")
        // order: expose as Python list [0, 1, 2, 3]
        .def_property("order",
            [](vr::LayoutState& s) -> std::vector<int> {
                return {s.order[0], s.order[1], s.order[2], s.order[3]};
            },
            [](vr::LayoutState& s, const std::vector<int>& v) {
                for (size_t i = 0; i < 4 && i < v.size(); ++i) {
                    s.order[i] = v[i];
                }
            },
            "Track display order as list of track indices");

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
        .def("initialize", &vr::Renderer::initialize, py::arg("config"))
        .def("shutdown", &vr::Renderer::shutdown)
        .def("play", &vr::Renderer::play)
        .def("pause", &vr::Renderer::pause)
        .def("seek", py::overload_cast<int64_t, vr::SeekType>(&vr::Renderer::seek),
             py::arg("target_pts_us"), py::arg("type") = vr::SeekType::Keyframe)
        .def("set_speed", &vr::Renderer::set_speed, py::arg("speed"))
        .def("step_forward", &vr::Renderer::step_forward)
        .def("step_backward", &vr::Renderer::step_backward)
        .def("is_playing", &vr::Renderer::is_playing)
        .def("is_initialized", &vr::Renderer::is_initialized)
        .def("current_pts_us", &vr::Renderer::current_pts_us)
        .def("current_speed", &vr::Renderer::current_speed)
        .def("track_count", &vr::Renderer::track_count)
        .def("duration_us", &vr::Renderer::duration_us)
        // Dynamic track management
        .def("add_track", &vr::Renderer::add_track, py::arg("video_path"),
             "Add a video track, returns slot index (0-3) or -1 on failure")
        .def("remove_track", &vr::Renderer::remove_track, py::arg("slot"),
             "Remove track at given slot")
        .def("has_track", &vr::Renderer::has_track, py::arg("slot"),
             "Check if slot has a track")
        // Layout control
        .def("apply_layout", &vr::Renderer::apply_layout, py::arg("state"),
             "Atomically apply layout state and trigger redraw if paused")
        .def("layout", &vr::Renderer::layout,
             "Get a snapshot of the current layout state");

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

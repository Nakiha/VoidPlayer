#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "voidview_native/hardware_decoder.hpp"

namespace py = pybind11;

PYBIND11_MODULE(voidview_native, m) {
    m.doc() = "VoidView Native Module - Hardware Accelerated Video Decoder";

    // Hardware type constants
    m.attr("HW_AUTO") = 0;
    m.attr("HW_D3D11VA") = 1;
    m.attr("HW_NVDEC") = 2;

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
             "Seek to timestamp in milliseconds")

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

#pragma once

#include <string>
#include <memory>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace voidview {

/**
 * Hardware-accelerated video decoder
 *
 * Wraps FFmpeg hardware decoding, supports D3D11VA and NVDEC.
 */
class HardwareDecoder {
public:
    explicit HardwareDecoder(const std::string& source_url);
    ~HardwareDecoder();

    // Disable copy
    HardwareDecoder(const HardwareDecoder&) = delete;
    HardwareDecoder& operator=(const HardwareDecoder&) = delete;

    // Allow move
    HardwareDecoder(HardwareDecoder&&) noexcept;
    HardwareDecoder& operator=(HardwareDecoder&&) noexcept;

    // ==================== Initialization ====================

    /**
     * Initialize decoder
     * @param hw_device_type Hardware type: 0=Auto, 1=D3D11VA, 2=NVDEC/CUDA
     * @return True on success
     */
    bool initialize(int hw_device_type = 0);

    /**
     * Set OpenGL context (for texture sharing)
     * @param gl_context QOpenGLContext pointer
     */
    void set_opengl_context(void* gl_context);

    // ==================== Decoding ====================

    /**
     * Decode next frame
     * @return True if new frame decoded
     */
    bool decode_next_frame();

    /**
     * Seek to timestamp
     * @param timestamp_ms Target time in milliseconds
     * @return True on success
     */
    bool seek_to(int64_t timestamp_ms);

    // ==================== Properties ====================

    /** Get OpenGL texture ID of current frame */
    uint32_t get_texture_id() const;

    /** Get current frame timestamp in milliseconds */
    int64_t get_current_pts_ms() const;

    /** Get total duration in milliseconds */
    int64_t get_duration_ms() const;

    /** Check if source is seekable */
    bool is_seekable() const;

    /** Check if end of file reached */
    bool is_eof() const;

    /** Check if error occurred */
    bool has_error() const;

    /** Get last error message */
    std::string get_error_message() const;

    /** Get video width */
    int get_width() const;

    /** Get video height */
    int get_height() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace voidview

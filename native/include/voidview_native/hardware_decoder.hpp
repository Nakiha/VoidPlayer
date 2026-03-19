#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include <functional>

#include "voidview_native/cancel_token.hpp"

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
     * Seek to timestamp (keyframe-level, fast)
     * @param timestamp_ms Target time in milliseconds
     * @return True on success
     */
    bool seek_to(int64_t timestamp_ms);

    /**
     * Seek to exact frame before timestamp (frame-accurate, slower)
     * Seeks to the last frame with pts <= timestamp_ms
     * @param timestamp_ms Target time in milliseconds
     * @return True on success
     */
    bool seek_to_precise(int64_t timestamp_ms);

    // ==================== Async/Cancellable API ====================

    /**
     * Decode next frame with cancellation support
     * @param cancel_token Token to check for cancellation
     * @return True if new frame decoded, false if cancelled or error
     */
    bool decode_next_frame_async(CancelToken& cancel_token);

    /**
     * Seek to exact frame with cancellation support
     * Checks cancel_token between decode iterations
     * @param timestamp_ms Target time in milliseconds
     * @param cancel_token Token to check for cancellation
     * @return True on success, false if cancelled
     */
    bool seek_to_precise_async(int64_t timestamp_ms, CancelToken& cancel_token);

    /**
     * Check if there's a pending decoded frame waiting for texture upload
     * Used in async mode: decode thread decodes, GL thread uploads
     */
    bool has_pending_frame() const;

    /**
     * Upload pending frame to texture (must be called in GL context)
     * @return True if upload succeeded
     */
    bool upload_pending_frame();

    // ==================== Internal API (for DecodeWorker) ====================

    /**
     * Decode next frame without GL upload (can be called from worker thread)
     * Sets has_pending_frame_ = true on success
     * @return True if new frame decoded
     */
    bool decode_frame_internal();

    /**
     * Seek to exact frame without GL upload (can be called from worker thread)
     * Sets has_pending_frame_ = true on success
     * @param timestamp_ms Target time in milliseconds
     * @return True on success
     */
    bool seek_to_precise_internal(int64_t timestamp_ms);

    /**
     * Seek to keyframe without GL upload (can be called from worker thread)
     * @param timestamp_ms Target time in milliseconds
     * @return True on success
     */
    bool seek_to_keyframe_internal(int64_t timestamp_ms);

    /**
     * Helper for precise seek: seek to frame before target
     * @param target_ms Target timestamp
     * @param known_frame_ms Known frame timestamp before target
     * @return True on success
     */
    bool seek_to_frame_before_internal(int64_t target_ms, int64_t known_frame_ms);

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

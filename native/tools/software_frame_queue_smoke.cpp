#include "video_renderer/decode/frame_timestamp_rescaler.h"
#include "video_renderer/decode/software_frame_publisher.h"

#include <array>
#include <cstdint>
#include <cstdio>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

AVFrame* make_limited_black_yuv420p_frame() {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return nullptr;
    }

    static std::array<uint8_t, 4> y = {16, 16, 16, 16};
    static std::array<uint8_t, 1> u = {128};
    static std::array<uint8_t, 1> v = {128};

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 2;
    frame->height = 2;
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->pts = 90;
    frame->pkt_dts = 45;
    frame->duration = 9;
    frame->data[0] = y.data();
    frame->data[1] = u.data();
    frame->data[2] = v.data();
    frame->linesize[0] = 2;
    frame->linesize[1] = 1;
    frame->linesize[2] = 1;
    return frame;
}

}  // namespace

int main() {
    AVFrame* frame = make_limited_black_yuv420p_frame();
    if (!frame) {
        return fail("failed to allocate frame");
    }

    vr::rescale_frame_timestamps_to_us(frame, AVRational{1, 90});

    vr::TrackBuffer buffer(2, 1);
    vr::SoftwareFrameQueuePublisher publisher(buffer);
    if (!publisher.publish_bgra_frame(frame)) {
        av_frame_free(&frame);
        return fail("failed to publish software frame");
    }
    av_frame_free(&frame);

    if (buffer.total_count() != 1) {
        return fail("published frame was not queued");
    }

    auto queued = buffer.peek();
    if (!queued) {
        return fail("queued frame is missing");
    }
    if (queued->storage_kind() != vr::FrameStorageKind::CpuRgba) {
        return fail("queued frame is not CpuRgba");
    }
    if (queued->pts_us != 1000000 || queued->dts_us != 500000 || queued->duration_us != 100000) {
        return fail("queued frame timestamps were not preserved in microseconds");
    }
    if (queued->width != 2 || queued->height != 2) {
        return fail("queued frame dimensions are wrong");
    }
    if (queued->color.range != vr::VIDEO_COLOR_RANGE_LIMITED ||
        queued->color.matrix != vr::VIDEO_COLOR_MATRIX_BT601 ||
        queued->color.transfer != vr::VIDEO_COLOR_TRANSFER_SDR ||
        queued->color.primaries != vr::VIDEO_COLOR_PRIMARIES_BT601) {
        return fail("queued frame color metadata was not preserved");
    }

    const auto* rgba = queued->cpu_rgba_storage();
    if (!rgba || !rgba->data || rgba->stride != 8 || rgba->data->size() != 16) {
        return fail("queued frame storage is malformed");
    }
    for (size_t i = 0; i < rgba->data->size(); i += 4) {
        if ((*rgba->data)[i] != 0 || (*rgba->data)[i + 1] != 0 ||
            (*rgba->data)[i + 2] != 0 || (*rgba->data)[i + 3] != 255) {
            return fail("queued frame pixel is not limited-range black BGRA");
        }
    }

    if (!buffer.advance()) {
        return fail("queued frame could not advance");
    }
    if (buffer.last_presented_pts_us() != 1000000) {
        return fail("TrackBuffer did not retain shared presentation pts semantics");
    }
    return 0;
}

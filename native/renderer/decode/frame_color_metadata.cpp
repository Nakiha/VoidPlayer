#include "renderer/decode/frame_color_metadata.h"

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace vr {
namespace {

VideoColorRange map_color_range(AVColorRange range) {
    switch (range) {
    case AVCOL_RANGE_JPEG:
        return VIDEO_COLOR_RANGE_FULL;
    case AVCOL_RANGE_MPEG:
        return VIDEO_COLOR_RANGE_LIMITED;
    default:
        return VIDEO_COLOR_RANGE_UNKNOWN;
    }
}

VideoColorMatrix map_color_matrix(AVColorSpace space) {
    switch (space) {
    case AVCOL_SPC_BT709:
        return VIDEO_COLOR_MATRIX_BT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
        return VIDEO_COLOR_MATRIX_BT601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return VIDEO_COLOR_MATRIX_BT2020_NCL;
    default:
        return VIDEO_COLOR_MATRIX_UNKNOWN;
    }
}

VideoColorTransfer map_color_transfer(AVColorTransferCharacteristic transfer) {
    switch (transfer) {
    case AVCOL_TRC_SMPTE2084:
        return VIDEO_COLOR_TRANSFER_PQ;
    case AVCOL_TRC_ARIB_STD_B67:
        return VIDEO_COLOR_TRANSFER_HLG;
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_GAMMA22:
    case AVCOL_TRC_GAMMA28:
    case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_SMPTE240M:
    case AVCOL_TRC_IEC61966_2_1:
    case AVCOL_TRC_BT2020_10:
    case AVCOL_TRC_BT2020_12:
        return VIDEO_COLOR_TRANSFER_SDR;
    default:
        return VIDEO_COLOR_TRANSFER_UNKNOWN;
    }
}

VideoColorPrimaries map_color_primaries(AVColorPrimaries primaries) {
    switch (primaries) {
    case AVCOL_PRI_BT709:
        return VIDEO_COLOR_PRIMARIES_BT709;
    case AVCOL_PRI_BT470BG:
    case AVCOL_PRI_SMPTE170M:
    case AVCOL_PRI_SMPTE240M:
        return VIDEO_COLOR_PRIMARIES_BT601;
    case AVCOL_PRI_BT2020:
        return VIDEO_COLOR_PRIMARIES_BT2020;
    default:
        return VIDEO_COLOR_PRIMARIES_UNKNOWN;
    }
}

void apply_color_defaults(VideoColorInfo& info, int width, int height) {
    // FFmpeg often leaves screen recordings partially unspecified. Pick the
    // same conservative defaults most players use for YUV video.
    if (info.range == VIDEO_COLOR_RANGE_UNKNOWN) {
        info.range = VIDEO_COLOR_RANGE_LIMITED;
    }
    if (info.matrix == VIDEO_COLOR_MATRIX_UNKNOWN) {
        info.matrix = width >= 1280 || height > 576
            ? VIDEO_COLOR_MATRIX_BT709
            : VIDEO_COLOR_MATRIX_BT601;
    }
    if (info.transfer == VIDEO_COLOR_TRANSFER_UNKNOWN) {
        info.transfer = VIDEO_COLOR_TRANSFER_SDR;
    }
    if (info.primaries == VIDEO_COLOR_PRIMARIES_UNKNOWN) {
        info.primaries = info.matrix == VIDEO_COLOR_MATRIX_BT2020_NCL
            ? VIDEO_COLOR_PRIMARIES_BT2020
            : (info.matrix == VIDEO_COLOR_MATRIX_BT601
                ? VIDEO_COLOR_PRIMARIES_BT601
                : VIDEO_COLOR_PRIMARIES_BT709);
    }
}

}  // namespace

VideoColorInfo color_info_from_av_frame(const AVFrame* frame) {
    VideoColorInfo info;
    if (!frame) {
        return info;
    }

    info.range = map_color_range(frame->color_range);
    info.matrix = map_color_matrix(frame->colorspace);
    info.transfer = map_color_transfer(frame->color_trc);
    info.primaries = map_color_primaries(frame->color_primaries);

    // FFmpeg often leaves screen recordings partially unspecified. Pick the
    // same conservative defaults most players use for YUV video.
    const auto format = static_cast<AVPixelFormat>(frame->format);
    if (info.range == VIDEO_COLOR_RANGE_UNKNOWN &&
        (format == AV_PIX_FMT_YUVJ420P ||
         format == AV_PIX_FMT_YUVJ422P ||
         format == AV_PIX_FMT_YUVJ444P)) {
        info.range = VIDEO_COLOR_RANGE_FULL;
    }
    apply_color_defaults(info, frame->width, frame->height);
    return info;
}

VideoColorInfo color_info_from_av_codec_parameters(const AVCodecParameters* params) {
    VideoColorInfo info;
    if (!params) {
        return info;
    }

    info.range = map_color_range(params->color_range);
    info.matrix = map_color_matrix(params->color_space);
    info.transfer = map_color_transfer(params->color_trc);
    info.primaries = map_color_primaries(params->color_primaries);
    apply_color_defaults(info, params->width, params->height);
    return info;
}

}  // namespace vr

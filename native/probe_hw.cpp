#include <stdio.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}
int main() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { printf("H264 not found\n"); return 1; }
    printf("H264 decoder: %s\n", codec->name);
    for (int i = 0; i < 20; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) break;
        printf("  hw[%d]: pix_fmt=%d (%s) methods=0x%x device_type=%d\n",
               i, cfg->pix_fmt, av_get_pix_fmt_name(cfg->pix_fmt),
               cfg->methods, cfg->device_type);
    }

    const AVCodec* hevc = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (hevc) {
        printf("HEVC decoder: %s\n", hevc->name);
        for (int i = 0; i < 20; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(hevc, i);
            if (!cfg) break;
            printf("  hw[%d]: pix_fmt=%d (%s) methods=0x%x device_type=%d\n",
                   i, cfg->pix_fmt, av_get_pix_fmt_name(cfg->pix_fmt),
                   cfg->methods, cfg->device_type);
        }
    }
    return 0;
}

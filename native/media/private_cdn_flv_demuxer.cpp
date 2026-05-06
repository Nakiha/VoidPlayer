#include "media/private_cdn_flv_demuxer.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

namespace vr {
namespace {

constexpr uint8_t kFlvTagAudio = 0x08;
constexpr uint8_t kFlvTagVideo = 0x09;
constexpr uint8_t kFlvCodecAac = 10 << 4;
constexpr uint8_t kFlvCodecMp3 = 2 << 4;
constexpr uint8_t kPrivateFlvAv1 = 13;
constexpr uint8_t kPrivateFlvVvc = 14;
constexpr int kVideoStreamIndex = 0;
constexpr int kAudioStreamIndex = 1;

int64_t read_flv_timestamp_ms(AVIOContext* pb) {
    const int64_t low = avio_rb24(pb);
    const int64_t high = avio_r8(pb);
    return low | (high << 24);
}

int32_t read_signed_24(AVIOContext* pb) {
    return static_cast<int32_t>((avio_rb24(pb) + 0xff800000) ^ 0xff800000);
}

bool read_flv_header(AVIOContext* pb, int64_t* first_tag_offset) {
    if (avio_r8(pb) != 'F' || avio_r8(pb) != 'L' || avio_r8(pb) != 'V') {
        return false;
    }
    avio_skip(pb, 2); // version + flags
    const int64_t data_offset = avio_rb32(pb);
    if (data_offset < 9) {
        return false;
    }
    *first_tag_offset = data_offset + 4; // PreviousTagSize0
    return avio_seek(pb, *first_tag_offset, SEEK_SET) >= 0;
}

bool is_private_video_flags(uint8_t flags) {
    if (flags & 0x80) {
        return false; // Enhanced FLV is handled by stock FFmpeg n8.1.
    }
    const uint8_t codec_id = flags & 0x0f;
    return codec_id == kPrivateFlvAv1 || codec_id == kPrivateFlvVvc;
}

int parse_aac_sample_rate(const uint8_t* data, int size) {
    static constexpr int kRates[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350,
    };
    if (size < 2) {
        return 0;
    }
    const int index = ((data[0] & 0x07) << 1) | ((data[1] & 0x80) >> 7);
    if (index < 0 || index >= static_cast<int>(sizeof(kRates) / sizeof(kRates[0]))) {
        return 0;
    }
    return kRates[index];
}

int parse_aac_channels(const uint8_t* data, int size) {
    if (size < 2) {
        return 0;
    }
    return (data[1] >> 3) & 0x0f;
}

int flv_audio_sample_rate(uint8_t flags) {
    switch ((flags >> 2) & 0x03) {
    case 0: return 5512;
    case 1: return 11025;
    case 2: return 22050;
    case 3: return 44100;
    default: return 0;
    }
}

uint16_t read_be16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

AVPixelFormat vvc_pix_fmt_from_config(int chroma_format_idc, int bit_depth_minus8) {
    const int bit_depth = bit_depth_minus8 + 8;
    switch (chroma_format_idc) {
    case 0:
        if (bit_depth == 8) return AV_PIX_FMT_GRAY8;
        if (bit_depth == 10) return AV_PIX_FMT_GRAY10LE;
        if (bit_depth == 12) return AV_PIX_FMT_GRAY12LE;
        break;
    case 1:
        if (bit_depth == 8) return AV_PIX_FMT_YUV420P;
        if (bit_depth == 10) return AV_PIX_FMT_YUV420P10LE;
        if (bit_depth == 12) return AV_PIX_FMT_YUV420P12LE;
        break;
    case 2:
        if (bit_depth == 8) return AV_PIX_FMT_YUV422P;
        if (bit_depth == 10) return AV_PIX_FMT_YUV422P10LE;
        if (bit_depth == 12) return AV_PIX_FMT_YUV422P12LE;
        break;
    case 3:
        if (bit_depth == 8) return AV_PIX_FMT_YUV444P;
        if (bit_depth == 10) return AV_PIX_FMT_YUV444P10LE;
        if (bit_depth == 12) return AV_PIX_FMT_YUV444P12LE;
        break;
    default:
        break;
    }
    return AV_PIX_FMT_NONE;
}

void apply_vvc_config_metadata(AVCodecParameters* par, const uint8_t* data, int size) {
    if (!par || !data || size < 4) {
        return;
    }

    int pos = 0;
    const uint8_t flags = data[pos++];
    const bool ptl_present = (flags & 0x01) != 0;

    if (pos + 3 > size) {
        return;
    }
    const uint16_t header = read_be16(data + pos);
    pos += 2;
    const int num_sublayers = (header >> 4) & 0x07;
    const int chroma_format_idc = header & 0x03;
    const int bit_depth_minus8 = (data[pos++] >> 5) & 0x07;

    const AVPixelFormat pix_fmt = vvc_pix_fmt_from_config(chroma_format_idc, bit_depth_minus8);
    if (pix_fmt != AV_PIX_FMT_NONE) {
        par->format = pix_fmt;
    }

    if (!ptl_present || pos >= size) {
        return;
    }

    const int num_bytes_constraint_info = data[pos++] & 0x3f;
    if (pos + 2 + num_bytes_constraint_info > size) {
        return;
    }
    pos += 2 + num_bytes_constraint_info;

    if (num_sublayers > 1) {
        if (pos >= size) {
            return;
        }
        const uint8_t present_flags = data[pos++];
        int present_count = 0;
        for (int i = num_sublayers - 2; i >= 0; --i) {
            if ((present_flags >> (7 - (num_sublayers - 2 - i))) & 0x01) {
                ++present_count;
            }
        }
        if (pos + present_count > size) {
            return;
        }
        pos += present_count;
    }

    if (pos >= size) {
        return;
    }
    const int num_sub_profiles = data[pos++];
    if (pos + num_sub_profiles * 4 + 6 > size) {
        return;
    }
    pos += num_sub_profiles * 4;

    const int width = read_be16(data + pos);
    pos += 2;
    const int height = read_be16(data + pos);
    if (width > 0 && height > 0) {
        par->width = width;
        par->height = height;
    }
}

} // namespace

PrivateCdnFlvDemuxer::~PrivateCdnFlvDemuxer() {
    close();
}

bool PrivateCdnFlvDemuxer::probe(const std::string& path) {
    AVIOContext* pb = nullptr;
    if (avio_open2(&pb, path.c_str(), AVIO_FLAG_READ, nullptr, nullptr) < 0) {
        return false;
    }

    int64_t tag_offset = 0;
    bool found = false;
    if (read_flv_header(pb, &tag_offset)) {
        for (int i = 0; i < 256 && !avio_feof(pb); ++i) {
            const int64_t tag_start = avio_tell(pb);
            const int tag_type = avio_r8(pb);
            if (tag_type < 0 || avio_feof(pb)) {
                break;
            }
            const int data_size = avio_rb24(pb);
            avio_skip(pb, 7); // timestamp + stream id
            const int64_t data_start = avio_tell(pb);
            if (data_size <= 0) {
                avio_seek(pb, data_start + data_size + 4, SEEK_SET);
                continue;
            }
            if ((tag_type & 0x1f) == kFlvTagVideo) {
                const uint8_t flags = static_cast<uint8_t>(avio_r8(pb));
                if (is_private_video_flags(flags)) {
                    found = true;
                    break;
                }
            }
            if (avio_seek(pb, data_start + data_size + 4, SEEK_SET) < 0 ||
                avio_tell(pb) <= tag_start) {
                break;
            }
        }
    }

    avio_closep(&pb);
    return found;
}

bool PrivateCdnFlvDemuxer::open(const std::string& path) {
    close();
    if (avio_open2(&pb_, path.c_str(), AVIO_FLAG_READ, nullptr, nullptr) < 0) {
        return false;
    }
    if (!scan()) {
        close();
        return false;
    }
    packet_cursor_ = 0;
    return true;
}

void PrivateCdnFlvDemuxer::close() {
    if (pb_) {
        avio_closep(&pb_);
    }
    reset_owned_params();
    packets_.clear();
    packet_cursor_ = 0;
    stats_ = DemuxStats{};
}

void PrivateCdnFlvDemuxer::reset_owned_params() {
    if (video_params_) {
        avcodec_parameters_free(&video_params_);
    }
    if (audio_params_) {
        avcodec_parameters_free(&audio_params_);
    }
}

void PrivateCdnFlvDemuxer::ensure_video_params(AVCodecID codec_id) {
    if (!video_params_) {
        video_params_ = avcodec_parameters_alloc();
        if (!video_params_) {
            return;
        }
        video_params_->codec_type = AVMEDIA_TYPE_VIDEO;
        stats_.video_stream_index = kVideoStreamIndex;
        stats_.time_base = AVRational{1, 1000};
        stats_.codec_params = video_params_;
        stats_.format_name = "Private CDN FLV";
    }
    video_params_->codec_id = codec_id;
    stats_.codec_name = avcodec_get_name(codec_id);
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(codec_id);
    if (descriptor && descriptor->long_name) {
        stats_.codec_long_name = descriptor->long_name;
    }
}

void PrivateCdnFlvDemuxer::ensure_aac_params() {
    if (!audio_params_) {
        audio_params_ = avcodec_parameters_alloc();
        if (!audio_params_) {
            return;
        }
        audio_params_->codec_type = AVMEDIA_TYPE_AUDIO;
        audio_params_->codec_id = AV_CODEC_ID_AAC;
        stats_.audio_stream_index = kAudioStreamIndex;
        stats_.audio_time_base = AVRational{1, 1000};
        stats_.audio_codec_params = audio_params_;
    }
}

void PrivateCdnFlvDemuxer::ensure_mp3_params(int sample_rate, int channels) {
    if (!audio_params_) {
        audio_params_ = avcodec_parameters_alloc();
        if (!audio_params_) {
            return;
        }
        audio_params_->codec_type = AVMEDIA_TYPE_AUDIO;
        audio_params_->codec_id = AV_CODEC_ID_MP3;
        stats_.audio_stream_index = kAudioStreamIndex;
        stats_.audio_time_base = AVRational{1, 1000};
        stats_.audio_codec_params = audio_params_;
    }
    audio_params_->sample_rate = sample_rate;
    av_channel_layout_uninit(&audio_params_->ch_layout);
    av_channel_layout_default(&audio_params_->ch_layout, channels);
    stats_.sample_rate = sample_rate;
    stats_.channels = channels;
}

bool PrivateCdnFlvDemuxer::copy_extradata(AVCodecParameters* par, int64_t offset, int size) {
    if (!par || size <= 0 || avio_seek(pb_, offset, SEEK_SET) < 0) {
        return false;
    }
    uint8_t* data = static_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!data) {
        return false;
    }
    const int read = avio_read(pb_, data, size);
    if (read != size) {
        av_free(data);
        return false;
    }
    av_freep(&par->extradata);
    par->extradata = data;
    par->extradata_size = size;
    if (par->codec_id == AV_CODEC_ID_VVC) {
        apply_vvc_config_metadata(par, data, size);
        if (par == video_params_) {
            stats_.width = par->width;
            stats_.height = par->height;
        }
    }
    return true;
}

bool PrivateCdnFlvDemuxer::scan() {
    int64_t tag_offset = 0;
    if (!read_flv_header(pb_, &tag_offset)) {
        return false;
    }

    int64_t max_ts_ms = 0;
    while (!avio_feof(pb_)) {
        const int64_t tag_start = avio_tell(pb_);
        const int tag_type = avio_r8(pb_);
        if (tag_type < 0 || avio_feof(pb_)) {
            break;
        }
        const int data_size = avio_rb24(pb_);
        const int64_t dts_ms = read_flv_timestamp_ms(pb_);
        avio_skip(pb_, 3); // stream id
        const int64_t data_start = avio_tell(pb_);
        const int64_t next_tag = data_start + data_size + 4;
        if (data_size <= 0) {
            avio_seek(pb_, next_tag, SEEK_SET);
            continue;
        }

        if ((tag_type & 0x1f) == kFlvTagVideo) {
            const uint8_t flags = static_cast<uint8_t>(avio_r8(pb_));
            const uint8_t codec_id = flags & 0x0f;
            if (is_private_video_flags(flags) && data_size >= 5) {
                const AVCodecID ff_codec = codec_id == kPrivateFlvAv1
                    ? AV_CODEC_ID_AV1
                    : AV_CODEC_ID_VVC;
                ensure_video_params(ff_codec);

                const uint8_t packet_type = static_cast<uint8_t>(avio_r8(pb_));
                const int32_t cts_ms = read_signed_24(pb_);
                const int64_t payload_offset = avio_tell(pb_);
                const int payload_size = data_size - 5;
                if (packet_type == 0) {
                    int64_t extradata_offset = payload_offset;
                    int extradata_size = payload_size;
                    if (ff_codec == AV_CODEC_ID_AV1 && extradata_size > 4) {
                        extradata_offset += 4;
                        extradata_size -= 4;
                    }
                    copy_extradata(video_params_, extradata_offset, extradata_size);
                } else if (packet_type == 1 && payload_size > 0) {
                    PacketRecord rec;
                    rec.stream_index = kVideoStreamIndex;
                    rec.tag_offset = tag_start;
                    rec.payload_offset = payload_offset;
                    rec.payload_size = payload_size;
                    rec.dts_ms = dts_ms;
                    rec.pts_ms = dts_ms + cts_ms;
                    rec.keyframe = ((flags >> 4) & 0x0f) == 1;
                    packets_.push_back(rec);
                    max_ts_ms = std::max(max_ts_ms, rec.pts_ms);
                }
            }
        } else if ((tag_type & 0x1f) == kFlvTagAudio) {
            const uint8_t flags = static_cast<uint8_t>(avio_r8(pb_));
            const uint8_t codec_id = flags & 0xf0;
            if (codec_id == kFlvCodecAac && data_size >= 2) {
                ensure_aac_params();
                const uint8_t packet_type = static_cast<uint8_t>(avio_r8(pb_));
                const int64_t payload_offset = avio_tell(pb_);
                const int payload_size = data_size - 2;
                if (packet_type == 0) {
                    if (copy_extradata(audio_params_, payload_offset, payload_size)) {
                        audio_params_->sample_rate = parse_aac_sample_rate(
                            audio_params_->extradata, audio_params_->extradata_size);
                        const int channels = parse_aac_channels(
                            audio_params_->extradata, audio_params_->extradata_size);
                        if (channels > 0) {
                            av_channel_layout_uninit(&audio_params_->ch_layout);
                            av_channel_layout_default(&audio_params_->ch_layout, channels);
                            stats_.channels = channels;
                        }
                        stats_.sample_rate = audio_params_->sample_rate;
                    }
                } else if (packet_type == 1 && payload_size > 0) {
                    packets_.push_back(PacketRecord{
                        kAudioStreamIndex, tag_start, payload_offset, payload_size,
                        dts_ms, dts_ms, 0, true});
                    max_ts_ms = std::max(max_ts_ms, dts_ms);
                }
            } else if (codec_id == kFlvCodecMp3 && data_size > 1) {
                ensure_mp3_params(flv_audio_sample_rate(flags), (flags & 0x01) ? 2 : 1);
                packets_.push_back(PacketRecord{
                    kAudioStreamIndex, tag_start, data_start + 1, data_size - 1,
                    dts_ms, dts_ms, 0, true});
                max_ts_ms = std::max(max_ts_ms, dts_ms);
            }
        }

        if (avio_seek(pb_, next_tag, SEEK_SET) < 0 || avio_tell(pb_) <= tag_start) {
            break;
        }
    }

    if (!video_params_ || packets_.empty()) {
        return false;
    }

    finalize_packet_durations();
    stats_.duration_us = max_ts_ms > 0 ? max_ts_ms * 1000 : 0;
    return true;
}

void PrivateCdnFlvDemuxer::finalize_packet_durations() {
    for (size_t i = 0; i < packets_.size(); ++i) {
        for (size_t j = i + 1; j < packets_.size(); ++j) {
            if (packets_[j].stream_index == packets_[i].stream_index &&
                packets_[j].dts_ms > packets_[i].dts_ms) {
                packets_[i].duration_ms = packets_[j].dts_ms - packets_[i].dts_ms;
                break;
            }
        }
    }
}

int PrivateCdnFlvDemuxer::read_packet(AVPacket* pkt) {
    if (!pb_ || !pkt) {
        return AVERROR(EINVAL);
    }
    if (packet_cursor_ >= packets_.size()) {
        return AVERROR_EOF;
    }

    const PacketRecord& rec = packets_[packet_cursor_++];
    int ret = av_new_packet(pkt, rec.payload_size);
    if (ret < 0) {
        return ret;
    }
    if (avio_seek(pb_, rec.payload_offset, SEEK_SET) < 0 ||
        avio_read(pb_, pkt->data, rec.payload_size) != rec.payload_size) {
        av_packet_unref(pkt);
        return AVERROR(EIO);
    }

    pkt->stream_index = rec.stream_index;
    pkt->pos = rec.tag_offset;
    pkt->dts = rec.dts_ms;
    pkt->pts = rec.pts_ms;
    pkt->duration = rec.duration_ms;
    if (rec.keyframe) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }
    return 0;
}

int PrivateCdnFlvDemuxer::seek(int stream_index, int64_t timestamp_ms) {
    if (packets_.empty()) {
        return AVERROR_EOF;
    }

    int64_t anchor_ms = 0;
    bool found_keyframe = false;
    if (stream_index == kVideoStreamIndex || stats_.video_stream_index >= 0) {
        for (const PacketRecord& rec : packets_) {
            if (rec.stream_index == kVideoStreamIndex && rec.keyframe &&
                rec.dts_ms <= timestamp_ms) {
                anchor_ms = rec.dts_ms;
                found_keyframe = true;
            }
        }
    }
    if (!found_keyframe) {
        anchor_ms = timestamp_ms;
    }

    auto it = std::find_if(packets_.begin(), packets_.end(),
        [anchor_ms](const PacketRecord& rec) { return rec.dts_ms >= anchor_ms; });
    packet_cursor_ = it == packets_.end()
        ? packets_.size()
        : static_cast<size_t>(std::distance(packets_.begin(), it));
    return 0;
}

void PrivateCdnFlvDemuxer::flush() {
}

AVRational PrivateCdnFlvDemuxer::time_base_for_stream(int stream_index) const {
    if (stream_index == stats_.video_stream_index) {
        return stats_.time_base;
    }
    if (stream_index == stats_.audio_stream_index) {
        return stats_.audio_time_base;
    }
    return AVRational{0, 1};
}

} // namespace vr

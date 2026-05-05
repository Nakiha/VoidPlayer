#include "analysis/generators/analysis_generator.h"

#include "analysis/generators/bitstream_indexer.h"
#include "analysis/parsers/binary_types.h"
#include "common/win_utf8.h"
#include "media/private_cdn_flv_demuxer.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <vector>

namespace vr::analysis {

namespace {

struct FfmpegOpenTimeout {
    int64_t deadline_ns = 0;
};

int64_t steady_clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int ffmpeg_interrupt_callback(void* opaque) {
    auto* timeout = static_cast<FfmpegOpenTimeout*>(opaque);
    if (!timeout || timeout->deadline_ns <= 0) {
        return 0;
    }
    return steady_clock_ns() > timeout->deadline_ns ? 1 : 0;
}

AVFormatContext* alloc_format_context_with_timeout(FfmpegOpenTimeout& timeout,
                                                   std::chrono::seconds duration) {
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        return nullptr;
    }
    timeout.deadline_ns = steady_clock_ns() +
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    fmt_ctx->interrupt_callback.callback = &ffmpeg_interrupt_callback;
    fmt_ctx->interrupt_callback.opaque = &timeout;
    return fmt_ctx;
}

} // namespace

class OutputBudget {
public:
    explicit OutputBudget(uint64_t max_bytes) : max_bytes_(max_bytes) {}

    bool reserve(uint64_t bytes) {
        if (max_bytes_ > 0 && bytes > max_bytes_ - used_) {
            spdlog::warn("[AnalysisGen] output limit exceeded: used={} add={} max={}",
                         used_, bytes, max_bytes_);
            return false;
        }
        used_ += bytes;
        return true;
    }

private:
    uint64_t max_bytes_ = 0;
    uint64_t used_ = 0;
};

class VbtStreamWriter {
public:
    bool open(const std::string& path,
              int32_t time_base_num,
              int32_t time_base_den,
              OutputBudget* budget = nullptr) {
        path_ = path;
        budget_ = budget;
        if (budget_ && !budget_->reserve(sizeof(VbtHeader))) return false;
        out_.open(win_utf8::path_from_utf8(path), std::ios::binary);
        if (!out_) return false;

        header_ = {};
        header_.magic[0] = 'V'; header_.magic[1] = 'B';
        header_.magic[2] = 'T'; header_.magic[3] = '1';
        header_.time_base_num = time_base_num;
        header_.time_base_den = time_base_den;
        out_.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
        return out_.good();
    }

    bool append(const VbtEntry& entry) {
        if (!out_ || count_ == UINT32_MAX) return false;
        if (budget_ && !budget_->reserve(sizeof(VbtEntry))) return false;
        out_.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
        if (!out_) return false;
        ++count_;
        return true;
    }

    bool finish() {
        if (!out_) return false;
        header_.num_packets = count_;
        out_.seekp(0, std::ios::beg);
        out_.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
        out_.close();
        return !out_.fail();
    }

    void close() {
        if (out_.is_open()) out_.close();
    }

    uint32_t count() const { return count_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::ofstream out_;
    VbtHeader header_{};
    uint32_t count_ = 0;
    OutputBudget* budget_ = nullptr;
};

class VbiStreamWriter {
public:
    bool open(const std::string& path,
              VbiCodec codec,
              VbiUnitKind unit_kind,
              OutputBudget* budget = nullptr) {
        path_ = path;
        budget_ = budget;
        if (budget_ && !budget_->reserve(sizeof(VbiHeader))) return false;
        out_.open(win_utf8::path_from_utf8(path), std::ios::binary);
        if (!out_) return false;

        header_ = {};
        header_.magic[0] = 'V'; header_.magic[1] = 'B';
        header_.magic[2] = 'I'; header_.magic[3] = '2';
        header_.version = 2;
        header_.codec = static_cast<uint16_t>(codec);
        header_.unit_kind = static_cast<uint16_t>(unit_kind);
        header_.header_size = sizeof(VbiHeader);
        out_.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
        return out_.good();
    }

    bool append(const VbiEntry& entry) {
        if (!out_ || count_ == UINT32_MAX) return false;
        if (budget_ && !budget_->reserve(sizeof(VbiEntry))) return false;
        out_.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
        if (!out_) return false;
        ++count_;
        source_size_ = std::max(source_size_, entry.offset + entry.size);
        return true;
    }

    bool append_all(const std::vector<VbiEntry>& entries) {
        for (const auto& entry : entries) {
            if (!append(entry)) return false;
        }
        return true;
    }

    bool finish() {
        if (!out_) return false;
        header_.num_units = count_;
        header_.source_size = source_size_;
        out_.seekp(0, std::ios::beg);
        out_.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
        out_.close();
        return !out_.fail();
    }

    void close() {
        if (out_.is_open()) out_.close();
    }

    uint32_t count() const { return count_; }
    uint64_t source_size() const { return source_size_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::ofstream out_;
    VbiHeader header_{};
    uint32_t count_ = 0;
    uint64_t source_size_ = 0;
    OutputBudget* budget_ = nullptr;
};

static bool generateRawOnly(const std::string& video_path,
                            const std::string& vbi_path,
                            const std::string& vbt_path,
                            uint64_t max_output_bytes = 0) {
    VbiCodec codec = BitstreamIndexer::codec_from_path(video_path);
    if (codec == VbiCodec::Unknown) {
        return false;
    }

    int32_t tb_num = 1;
    int32_t tb_den = 60;
    VbtStreamWriter vbt_writer;
    VbiStreamWriter vbi_writer;
    OutputBudget budget(max_output_bytes);
    if (!vbt_writer.open(vbt_path, tb_num, tb_den, &budget)) {
        return false;
    }
    if (!vbi_writer.open(vbi_path, codec, BitstreamIndexer::unit_kind_for_codec(codec), &budget)) {
        vbt_writer.close();
        return false;
    }

    int32_t poc = 0;
    const bool indexed = BitstreamIndexer::index_raw_file_streaming(
        video_path,
        codec,
        [&](const VbiEntry& unit) {
            if (!vbi_writer.append(unit)) return false;
            if ((unit.flags & VBI_FLAG_IS_VCL) == 0) return true;

            VbtEntry entry{};
            entry.pts = poc;
            entry.dts = poc;
            entry.poc = poc++;
            entry.size = unit.size;
            entry.duration = 1;
            entry.flags = (unit.flags & VBI_FLAG_IS_KEYFRAME) ? VBT_FLAG_KEYFRAME : 0;
            return vbt_writer.append(entry);
        });

    if (!indexed) {
        vbt_writer.close();
        vbi_writer.close();
        return false;
    }

    if (vbt_writer.count() == 0) {
        spdlog::warn("[AnalysisGen] raw fallback found no coded units: {}", video_path);
    }

    const bool vbt_ok = vbt_writer.finish();
    const bool vbi_ok = vbi_writer.finish();
    return vbt_ok && vbi_ok;
}

static const char* annex_b_bsf_name(VbiCodec codec) {
    switch (codec) {
    case VbiCodec::H264: return "h264_mp4toannexb";
    case VbiCodec::HEVC: return "hevc_mp4toannexb";
    case VbiCodec::VVC:  return "vvc_mp4toannexb";
    default:             return nullptr;
    }
}

static bool requires_annex_b_filter(VbiCodec codec) {
    return codec == VbiCodec::H264 || codec == VbiCodec::HEVC || codec == VbiCodec::VVC;
}

static bool packet_starts_with_annex_b(const AVPacket* pkt) {
    if (!pkt || !pkt->data || pkt->size < 3) return false;
    const auto* data = pkt->data;
    const int len = pkt->size;
    return (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) ||
           (data[0] == 0 && data[1] == 0 && data[2] == 1);
}

static bool append_packet_if_safe(const AVPacket* pkt,
                                  VbiCodec codec,
                                  VbiStreamWriter& writer,
                                  bool& unsafe_fallback_warned) {
    if (!pkt || !pkt->data || pkt->size <= 0) return true;

    if (requires_annex_b_filter(codec) && !packet_starts_with_annex_b(pkt)) {
        if (!unsafe_fallback_warned) {
            spdlog::warn(
                "[AnalysisGen] skipping length-prefixed packets without Annex-B BSF; VBI unavailable for this stream");
            unsafe_fallback_warned = true;
        }
        return true;
    }

    uint64_t source_size = writer.source_size();
    return BitstreamIndexer::append_packet_streaming(
        codec,
        pkt->data,
        pkt->size,
        (pkt->flags & AV_PKT_FLAG_KEY) != 0,
        source_size,
        [&writer](const VbiEntry& entry) {
            return writer.append(entry);
        });
}

static AVBSFContext* create_annex_b_bsf(const AVCodecParameters* codecpar,
                                        AVRational time_base,
                                        VbiCodec codec) {
    const char* name = annex_b_bsf_name(codec);
    if (!name || !codecpar) return nullptr;

    const AVBitStreamFilter* filter = av_bsf_get_by_name(name);
    if (!filter) {
        spdlog::warn("[AnalysisGen] bitstream filter {} unavailable; parsing packets directly", name);
        return nullptr;
    }

    AVBSFContext* bsf = nullptr;
    int ret = av_bsf_alloc(filter, &bsf);
    if (ret < 0 || !bsf) {
        spdlog::warn("[AnalysisGen] av_bsf_alloc({}) failed: {:#x}", name, static_cast<unsigned>(ret));
        return nullptr;
    }

    ret = avcodec_parameters_copy(bsf->par_in, codecpar);
    if (ret < 0) {
        spdlog::warn("[AnalysisGen] avcodec_parameters_copy({}) failed: {:#x}",
                     name, static_cast<unsigned>(ret));
        av_bsf_free(&bsf);
        return nullptr;
    }
    bsf->time_base_in = time_base;

    ret = av_bsf_init(bsf);
    if (ret < 0) {
        spdlog::warn("[AnalysisGen] av_bsf_init({}) failed: {:#x}", name, static_cast<unsigned>(ret));
        av_bsf_free(&bsf);
        return nullptr;
    }

    spdlog::info("[AnalysisGen] using {} for VBI Annex-B indexing", name);
    return bsf;
}

static AVBSFContext* create_annex_b_bsf(AVStream* stream, VbiCodec codec) {
    if (!stream) return nullptr;
    return create_annex_b_bsf(stream->codecpar, stream->time_base, codec);
}

static bool append_filtered_packets(AVBSFContext* bsf,
                                    AVPacket* filtered_pkt,
                                    VbiCodec codec,
                                    VbiStreamWriter& writer) {
    if (!bsf || !filtered_pkt) return true;
    while (true) {
        int ret = av_bsf_receive_packet(bsf, filtered_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            spdlog::warn("[AnalysisGen] av_bsf_receive_packet failed: {:#x}",
                         static_cast<unsigned>(ret));
            break;
        }
        if (filtered_pkt->data && filtered_pkt->size > 0) {
            uint64_t source_size = writer.source_size();
            const bool ok = BitstreamIndexer::append_packet_streaming(
                codec,
                filtered_pkt->data,
                filtered_pkt->size,
                (filtered_pkt->flags & AV_PKT_FLAG_KEY) != 0,
                source_size,
                [&writer](const VbiEntry& entry) {
                    return writer.append(entry);
                });
            if (!ok) {
                av_packet_unref(filtered_pkt);
                return false;
            }
        }
        av_packet_unref(filtered_pkt);
    }
    return true;
}

static bool generatePrivateCdnFlv(const std::string& video_path,
                                  const std::string& vbi_path,
                                  const std::string& vbt_path,
                                  uint64_t max_output_bytes) {
    vr::PrivateCdnFlvDemuxer demuxer;
    if (!demuxer.open(video_path)) {
        return false;
    }

    const DemuxStats& stats = demuxer.stats();
    if (!stats.codec_params || stats.video_stream_index < 0) {
        return false;
    }

    const AVRational time_base = stats.time_base.num > 0 && stats.time_base.den > 0
        ? stats.time_base
        : AVRational{1, 1000};
    const VbiCodec codec = BitstreamIndexer::codec_from_ffmpeg_id(
        stats.codec_params->codec_id);
    if (codec == VbiCodec::Unknown) {
        return false;
    }

    spdlog::info("[AnalysisGen] using private CDN FLV demuxer: time_base={}/{}, codec={}",
                 time_base.num, time_base.den, static_cast<int>(codec));

    OutputBudget budget(max_output_bytes);
    VbtStreamWriter vbt_writer;
    if (!vbt_writer.open(vbt_path, time_base.num, time_base.den, &budget)) {
        return false;
    }
    VbiStreamWriter vbi_writer;
    if (!vbi_writer.open(vbi_path, codec, BitstreamIndexer::unit_kind_for_codec(codec), &budget)) {
        vbt_writer.close();
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    AVPacket* filtered_pkt = av_packet_alloc();
    if (!pkt || !filtered_pkt) {
        vbt_writer.close();
        vbi_writer.close();
        av_packet_free(&filtered_pkt);
        av_packet_free(&pkt);
        return false;
    }

    int32_t seq_poc = 0;
    bool scan_failed = false;
    bool unsafe_fallback_warned = false;
    AVBSFContext* annex_b_bsf = create_annex_b_bsf(stats.codec_params, time_base, codec);

    while (true) {
        int ret = demuxer.read_packet(pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) break;
            spdlog::warn("[AnalysisGen] private CDN FLV read error: {:#x}",
                         static_cast<unsigned>(ret));
            break;
        }

        if (pkt->stream_index != stats.video_stream_index) {
            av_packet_unref(pkt);
            continue;
        }

        VbtEntry entry{};
        entry.pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : 0;
        entry.dts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : 0;
        entry.poc = seq_poc++;
        entry.size = static_cast<uint32_t>(pkt->size);
        entry.duration = static_cast<uint32_t>(pkt->duration);
        entry.flags = (pkt->flags & AV_PKT_FLAG_KEY) ? VBT_FLAG_KEYFRAME : 0;
        std::memset(entry.reserved, 0, sizeof(entry.reserved));
        if (!vbt_writer.append(entry)) {
            scan_failed = true;
            av_packet_unref(pkt);
            break;
        }

        bool vbi_ok = true;
        if (annex_b_bsf) {
            int send_ret = av_bsf_send_packet(annex_b_bsf, pkt);
            if (send_ret >= 0) {
                vbi_ok = append_filtered_packets(
                    annex_b_bsf, filtered_pkt, codec, vbi_writer);
            } else {
                spdlog::warn("[AnalysisGen] private CDN FLV BSF send failed: {:#x}",
                             static_cast<unsigned>(send_ret));
                vbi_ok = append_packet_if_safe(
                    pkt, codec, vbi_writer, unsafe_fallback_warned);
            }
        } else {
            vbi_ok = append_packet_if_safe(
                pkt, codec, vbi_writer, unsafe_fallback_warned);
        }

        if (!vbi_ok) {
            scan_failed = true;
            av_packet_unref(pkt);
            break;
        }
        av_packet_unref(pkt);
    }

    if (annex_b_bsf) {
        av_bsf_send_packet(annex_b_bsf, nullptr);
        if (!append_filtered_packets(annex_b_bsf, filtered_pkt, codec, vbi_writer)) {
            scan_failed = true;
        }
    }

    if (annex_b_bsf) av_bsf_free(&annex_b_bsf);
    av_packet_free(&filtered_pkt);
    av_packet_free(&pkt);

    if (scan_failed || vbt_writer.count() == 0) {
        vbt_writer.close();
        vbi_writer.close();
        return false;
    }

    spdlog::info("[AnalysisGen] private CDN FLV scanned {} packets, {} bitstream units",
                 vbt_writer.count(), vbi_writer.count());
    const bool vbt_ok = vbt_writer.finish();
    const bool vbi_ok = vbi_writer.finish();
    return vbt_ok && vbi_ok;
}

// ---------------------------------------------------------------
// Public API
// ---------------------------------------------------------------

bool AnalysisGenerator::generate(const std::string& video_path,
                                 const std::string& vbi_path,
                                 const std::string& vbt_path,
                                 uint64_t max_output_bytes) {
    if (vr::PrivateCdnFlvDemuxer::probe(video_path)) {
        if (generatePrivateCdnFlv(video_path, vbi_path, vbt_path, max_output_bytes)) {
            return true;
        }
        spdlog::warn("[AnalysisGen] private CDN FLV fallback failed, trying stock FFmpeg path");
    }

    FfmpegOpenTimeout timeout;
    AVFormatContext* fmt_ctx = alloc_format_context_with_timeout(timeout, std::chrono::seconds(30));
    if (!fmt_ctx) {
        spdlog::error("[AnalysisGen] failed to allocate format context");
        return generateRawOnly(video_path, vbi_path, vbt_path, max_output_bytes);
    }
    int ret = avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        timeout.deadline_ns = 0;
        spdlog::error("[AnalysisGen] avformat_open_input failed: {:#x}", static_cast<unsigned>(ret));
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        return generateRawOnly(video_path, vbi_path, vbt_path, max_output_bytes);
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    timeout.deadline_ns = 0;
    if (ret < 0) {
        spdlog::error("[AnalysisGen] avformat_find_stream_info failed: {:#x}", static_cast<unsigned>(ret));
        avformat_close_input(&fmt_ctx);
        return generateRawOnly(video_path, vbi_path, vbt_path, max_output_bytes);
    }

    // Find video stream
    int video_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = static_cast<int>(i);
            break;
        }
    }
    if (video_idx < 0) {
        spdlog::error("[AnalysisGen] no video stream found");
        avformat_close_input(&fmt_ctx);
        return generateRawOnly(video_path, vbi_path, vbt_path, max_output_bytes);
    }

    AVRational time_base = fmt_ctx->streams[video_idx]->time_base;
    int32_t tb_num = time_base.num;
    int32_t tb_den = time_base.den;
    VbiCodec codec = BitstreamIndexer::codec_from_ffmpeg_id(
        fmt_ctx->streams[video_idx]->codecpar->codec_id);
    if (codec == VbiCodec::Unknown) {
        codec = BitstreamIndexer::codec_from_path(video_path);
    }

    spdlog::info("[AnalysisGen] video stream {}: time_base={}/{}, codec={}",
                 video_idx, tb_num, tb_den, static_cast<int>(codec));

    // Single-pass: append VBI and VBT data as it is discovered. Headers are
    // patched with final counts after the scan completes.
    OutputBudget budget(max_output_bytes);
    VbtStreamWriter vbt_writer;
    if (!vbt_writer.open(vbt_path, tb_num, tb_den, &budget)) {
        avformat_close_input(&fmt_ctx);
        return false;
    }
    VbiStreamWriter vbi_writer;
    if (!vbi_writer.open(vbi_path, codec, BitstreamIndexer::unit_kind_for_codec(codec), &budget)) {
        vbt_writer.close();
        avformat_close_input(&fmt_ctx);
        return false;
    }

    int32_t seq_poc = 0;         // Sequential POC for VBT
    bool unsafe_fallback_warned = false;
    AVBSFContext* annex_b_bsf = create_annex_b_bsf(fmt_ctx->streams[video_idx], codec);

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        vbt_writer.close();
        vbi_writer.close();
        if (annex_b_bsf) av_bsf_free(&annex_b_bsf);
        avformat_close_input(&fmt_ctx);
        return false;
    }
    AVPacket* filtered_pkt = av_packet_alloc();
    if (!filtered_pkt) {
        vbt_writer.close();
        vbi_writer.close();
        av_packet_free(&pkt);
        if (annex_b_bsf) av_bsf_free(&annex_b_bsf);
        avformat_close_input(&fmt_ctx);
        return false;
    }

    bool scan_failed = false;
    while (true) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) break;
            spdlog::warn("[AnalysisGen] av_read_frame error: {:#x}", static_cast<unsigned>(ret));
            break;
        }

        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }

        // --- VBT: extract per-packet timestamp info ---
        {
            VbtEntry entry{};
            entry.pts      = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : 0;
            entry.dts      = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : 0;
            entry.poc      = seq_poc++;
            entry.size     = static_cast<uint32_t>(pkt->size);
            entry.duration = static_cast<uint32_t>(pkt->duration);
            entry.flags    = (pkt->flags & AV_PKT_FLAG_KEY) ? VBT_FLAG_KEYFRAME : 0;
            std::memset(entry.reserved, 0, sizeof(entry.reserved));
            if (!vbt_writer.append(entry)) {
                scan_failed = true;
                av_packet_unref(pkt);
                break;
            }
        }

        // --- VBI2: parse codec-specific bitstream units from packet data.
        // MP4 stores H.264/H.265/H.266 samples as length-prefixed NAL units,
        // with a per-stream length size in extradata. Route those codecs
        // through FFmpeg's Annex-B filters so VBI indexing sees stable start
        // codes and parameter sets instead of guessing the length field width.
        bool vbi_ok = true;
        if (annex_b_bsf) {
            int send_ret = av_bsf_send_packet(annex_b_bsf, pkt);
            if (send_ret >= 0) {
                vbi_ok = append_filtered_packets(
                    annex_b_bsf, filtered_pkt, codec, vbi_writer);
            } else {
                spdlog::warn("[AnalysisGen] av_bsf_send_packet failed: {:#x}; skipping unsafe packet fallback if needed",
                             static_cast<unsigned>(send_ret));
                vbi_ok = append_packet_if_safe(
                    pkt, codec, vbi_writer, unsafe_fallback_warned);
            }
        } else {
            vbi_ok = append_packet_if_safe(
                pkt, codec, vbi_writer, unsafe_fallback_warned);
        }

        if (!vbi_ok) {
            scan_failed = true;
            av_packet_unref(pkt);
            break;
        }

        av_packet_unref(pkt);
    }

    if (annex_b_bsf) {
        av_bsf_send_packet(annex_b_bsf, nullptr);
        if (!append_filtered_packets(annex_b_bsf, filtered_pkt, codec, vbi_writer)) {
            spdlog::warn("[AnalysisGen] failed to append flushed VBI packets");
            scan_failed = true;
        }
    }

    av_packet_free(&filtered_pkt);
    av_packet_free(&pkt);
    if (annex_b_bsf) av_bsf_free(&annex_b_bsf);
    avformat_close_input(&fmt_ctx);

    if (scan_failed) {
        vbt_writer.close();
        vbi_writer.close();
        return false;
    }

    if (vbt_writer.count() == 0) {
        vbt_writer.close();
        vbi_writer.close();
        if (generateRawOnly(video_path, vbi_path, vbt_path, max_output_bytes)) {
            spdlog::info("[AnalysisGen] raw fallback synthesized VBT/VBI from raw units");
            return true;
        }
        return false;
    }

    spdlog::info("[AnalysisGen] scanned {} packets, {} bitstream units",
                 vbt_writer.count(), vbi_writer.count());

    // Write VBT
    bool vbt_ok = vbt_writer.finish();
    if (!vbt_ok) {
        spdlog::error("[AnalysisGen] failed to write VBT: {}", vbt_path);
        return false;
    }
    spdlog::info("[AnalysisGen] wrote VBT: {} ({} entries)", vbt_path, vbt_writer.count());

    // Write VBI
    bool vbi_ok = vbi_writer.finish();
    if (!vbi_ok) {
        spdlog::error("[AnalysisGen] failed to write VBI: {}", vbi_path);
    } else {
        spdlog::info("[AnalysisGen] wrote VBI: {} ({} entries)", vbi_path, vbi_writer.count());
    }

    return vbt_ok && vbi_ok;
}

} // namespace vr::analysis

#include "analysis/generators/analysis_generator.h"

#include "analysis/generators/bitstream_indexer.h"
#include "analysis/parsers/binary_types.h"
#include "common/win_utf8.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

namespace vr::analysis {

// Write binary VBT file
static bool writeVbt(const std::string& path,
                     const std::vector<VbtEntry>& entries,
                     int32_t time_base_num, int32_t time_base_den) {
    std::ofstream out(win_utf8::path_from_utf8(path), std::ios::binary);
    if (!out) return false;

    VbtHeader hdr{};
    hdr.magic[0] = 'V'; hdr.magic[1] = 'B';
    hdr.magic[2] = 'T'; hdr.magic[3] = '1';
    hdr.num_packets   = static_cast<uint32_t>(entries.size());
    hdr.time_base_num = time_base_num;
    hdr.time_base_den = time_base_den;
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(VbtHeader));

    for (const auto& e : entries) {
        out.write(reinterpret_cast<const char*>(&e), sizeof(VbtEntry));
    }

    return out.good();
}

// Write binary VBI file
static bool writeVbi(const std::string& path,
                     const BitstreamIndex& index) {
    std::ofstream out(win_utf8::path_from_utf8(path), std::ios::binary);
    if (!out) return false;

    VbiHeader hdr{};
    hdr.magic[0] = 'V'; hdr.magic[1] = 'B';
    hdr.magic[2] = 'I'; hdr.magic[3] = '2';
    hdr.version     = 2;
    hdr.codec       = static_cast<uint16_t>(index.codec);
    hdr.unit_kind   = static_cast<uint16_t>(index.unit_kind);
    hdr.header_size = sizeof(VbiHeader);
    hdr.num_units   = static_cast<uint32_t>(index.entries.size());
    hdr.source_size = index.source_size;
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(VbiHeader));

    for (const auto& e : index.entries) {
        out.write(reinterpret_cast<const char*>(&e), sizeof(VbiEntry));
    }

    return out.good();
}

static void synthesize_vbt_from_index(const BitstreamIndex& index,
                                      std::vector<VbtEntry>& pkt_entries,
                                      int32_t& tb_num,
                                      int32_t& tb_den) {
    tb_num = 1;
    tb_den = 60;
    int32_t poc = 0;
    for (const auto& unit : index.entries) {
        if ((unit.flags & VBI_FLAG_IS_VCL) == 0) continue;
        VbtEntry entry{};
        entry.pts = poc;
        entry.dts = poc;
        entry.poc = poc++;
        entry.size = unit.size;
        entry.duration = 1;
        entry.flags = (unit.flags & VBI_FLAG_IS_KEYFRAME) ? VBT_FLAG_KEYFRAME : 0;
        pkt_entries.push_back(entry);
    }
}

static bool generateRawOnly(const std::string& video_path,
                            const std::string& vbi_path,
                            const std::string& vbt_path) {
    BitstreamIndex index;
    if (!BitstreamIndexer::index_raw_file(video_path, VbiCodec::Unknown, index)) {
        return false;
    }

    std::vector<VbtEntry> packets;
    int32_t tb_num = 1;
    int32_t tb_den = 60;
    synthesize_vbt_from_index(index, packets, tb_num, tb_den);
    if (packets.empty()) {
        spdlog::warn("[AnalysisGen] raw fallback found no coded units: {}", video_path);
    }

    if (!writeVbt(vbt_path, packets, tb_num, tb_den)) {
        return false;
    }
    return writeVbi(vbi_path, index);
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

static void append_packet_if_safe(const AVPacket* pkt,
                                  VbiCodec codec,
                                  BitstreamIndex& bitstream_index,
                                  bool& unsafe_fallback_warned) {
    if (!pkt || !pkt->data || pkt->size <= 0) return;

    if (requires_annex_b_filter(codec) && !packet_starts_with_annex_b(pkt)) {
        if (!unsafe_fallback_warned) {
            spdlog::warn(
                "[AnalysisGen] skipping length-prefixed packets without Annex-B BSF; VBI unavailable for this stream");
            unsafe_fallback_warned = true;
        }
        return;
    }

    BitstreamIndexer::append_packet(
        codec,
        pkt->data,
        pkt->size,
        (pkt->flags & AV_PKT_FLAG_KEY) != 0,
        bitstream_index);
}

static AVBSFContext* create_annex_b_bsf(AVStream* stream, VbiCodec codec) {
    const char* name = annex_b_bsf_name(codec);
    if (!name || !stream || !stream->codecpar) return nullptr;

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

    ret = avcodec_parameters_copy(bsf->par_in, stream->codecpar);
    if (ret < 0) {
        spdlog::warn("[AnalysisGen] avcodec_parameters_copy({}) failed: {:#x}",
                     name, static_cast<unsigned>(ret));
        av_bsf_free(&bsf);
        return nullptr;
    }
    bsf->time_base_in = stream->time_base;

    ret = av_bsf_init(bsf);
    if (ret < 0) {
        spdlog::warn("[AnalysisGen] av_bsf_init({}) failed: {:#x}", name, static_cast<unsigned>(ret));
        av_bsf_free(&bsf);
        return nullptr;
    }

    spdlog::info("[AnalysisGen] using {} for VBI Annex-B indexing", name);
    return bsf;
}

static void append_filtered_packets(AVBSFContext* bsf,
                                    AVPacket* filtered_pkt,
                                    VbiCodec codec,
                                    BitstreamIndex& bitstream_index) {
    if (!bsf || !filtered_pkt) return;
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
            BitstreamIndexer::append_packet(
                codec,
                filtered_pkt->data,
                filtered_pkt->size,
                (filtered_pkt->flags & AV_PKT_FLAG_KEY) != 0,
                bitstream_index);
        }
        av_packet_unref(filtered_pkt);
    }
}

// ---------------------------------------------------------------
// Public API
// ---------------------------------------------------------------

bool AnalysisGenerator::generate(const std::string& video_path,
                                 const std::string& vbi_path,
                                 const std::string& vbt_path) {
    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        spdlog::error("[AnalysisGen] avformat_open_input failed: {:#x}", static_cast<unsigned>(ret));
        return generateRawOnly(video_path, vbi_path, vbt_path);
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        spdlog::error("[AnalysisGen] avformat_find_stream_info failed: {:#x}", static_cast<unsigned>(ret));
        avformat_close_input(&fmt_ctx);
        return generateRawOnly(video_path, vbi_path, vbt_path);
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
        return generateRawOnly(video_path, vbi_path, vbt_path);
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

    // Single-pass: collect VBI and VBT data
    BitstreamIndex bitstream_index;
    bitstream_index.codec = codec;
    bitstream_index.unit_kind = BitstreamIndexer::unit_kind_for_codec(codec);
    std::vector<VbtEntry> pkt_entries;
    int32_t seq_poc = 0;         // Sequential POC for VBT
    bool unsafe_fallback_warned = false;
    AVBSFContext* annex_b_bsf = create_annex_b_bsf(fmt_ctx->streams[video_idx], codec);

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        if (annex_b_bsf) av_bsf_free(&annex_b_bsf);
        avformat_close_input(&fmt_ctx);
        return false;
    }
    AVPacket* filtered_pkt = av_packet_alloc();
    if (!filtered_pkt) {
        av_packet_free(&pkt);
        if (annex_b_bsf) av_bsf_free(&annex_b_bsf);
        avformat_close_input(&fmt_ctx);
        return false;
    }

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
            pkt_entries.push_back(entry);
        }

        // --- VBI2: parse codec-specific bitstream units from packet data.
        // MP4 stores H.264/H.265/H.266 samples as length-prefixed NAL units,
        // with a per-stream length size in extradata. Route those codecs
        // through FFmpeg's Annex-B filters so VBI indexing sees stable start
        // codes and parameter sets instead of guessing the length field width.
        if (annex_b_bsf) {
            int send_ret = av_bsf_send_packet(annex_b_bsf, pkt);
            if (send_ret >= 0) {
                append_filtered_packets(annex_b_bsf, filtered_pkt, codec, bitstream_index);
            } else {
                spdlog::warn("[AnalysisGen] av_bsf_send_packet failed: {:#x}; skipping unsafe packet fallback if needed",
                             static_cast<unsigned>(send_ret));
                append_packet_if_safe(pkt, codec, bitstream_index, unsafe_fallback_warned);
                av_packet_unref(pkt);
            }
        } else {
            append_packet_if_safe(pkt, codec, bitstream_index, unsafe_fallback_warned);
        }

        av_packet_unref(pkt);
    }

    if (annex_b_bsf) {
        av_bsf_send_packet(annex_b_bsf, nullptr);
        append_filtered_packets(annex_b_bsf, filtered_pkt, codec, bitstream_index);
    }

    av_packet_free(&filtered_pkt);
    av_packet_free(&pkt);
    if (annex_b_bsf) av_bsf_free(&annex_b_bsf);
    avformat_close_input(&fmt_ctx);

    if (pkt_entries.empty()) {
        BitstreamIndex raw_index;
        if (BitstreamIndexer::index_raw_file(video_path, codec, raw_index)) {
            bitstream_index = std::move(raw_index);
            synthesize_vbt_from_index(bitstream_index, pkt_entries, tb_num, tb_den);
            spdlog::info("[AnalysisGen] raw fallback synthesized {} packets from {} units",
                         pkt_entries.size(), bitstream_index.entries.size());
        }
    }

    spdlog::info("[AnalysisGen] scanned {} packets, {} bitstream units",
                 pkt_entries.size(), bitstream_index.entries.size());

    // Write VBT
    bool vbt_ok = writeVbt(vbt_path, pkt_entries, tb_num, tb_den);
    if (!vbt_ok) {
        spdlog::error("[AnalysisGen] failed to write VBT: {}", vbt_path);
        return false;
    }
    spdlog::info("[AnalysisGen] wrote VBT: {} ({} entries)", vbt_path, pkt_entries.size());

    // Write VBI
    bool vbi_ok = writeVbi(vbi_path, bitstream_index);
    if (!vbi_ok) {
        spdlog::warn("[AnalysisGen] failed to write VBI: {}", vbi_path);
        // VBI is optional — return true if VBT was written
    } else {
        spdlog::info("[AnalysisGen] wrote VBI: {} ({} entries)", vbi_path, bitstream_index.entries.size());
    }

    return vbt_ok;
}

} // namespace vr::analysis

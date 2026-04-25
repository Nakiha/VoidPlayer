#include "analysis/generators/analysis_generator.h"

#include "analysis/generators/bitstream_indexer.h"
#include "analysis/parsers/binary_types.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
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
    std::ofstream out(path, std::ios::binary);
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
    std::ofstream out(path, std::ios::binary);
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

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
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

        // --- VBI2: parse codec-specific bitstream units from packet data ---
        if (pkt->data && pkt->size > 0) {
            BitstreamIndexer::append_packet(
                codec,
                pkt->data,
                pkt->size,
                (pkt->flags & AV_PKT_FLAG_KEY) != 0,
                bitstream_index);
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
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

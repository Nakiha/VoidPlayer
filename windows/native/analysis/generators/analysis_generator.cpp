#include "analysis/generators/analysis_generator.h"

#include "analysis/parsers/binary_types.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <cstring>
#include <fstream>
#include <vector>

namespace vr::analysis {

// VVC NALU type classification (matches Python vvc_nalu_indexer.py exactly)
static constexpr uint8_t VBI_FLAG_IS_VCL      = 0x01;
static constexpr uint8_t VBI_FLAG_IS_SLICE    = 0x02;
static constexpr uint8_t VBI_FLAG_IS_KEYFRAME = 0x04;

static bool isVclType(uint8_t nal_type)  { return nal_type <= 11; }
static bool isSliceType(uint8_t nal_type) {
    return nal_type == 0 || nal_type == 1 || nal_type == 2 || nal_type == 3 ||
           nal_type == 7 || nal_type == 8 || nal_type == 9 || nal_type == 10;
}
static bool isKeyframeType(uint8_t nal_type) {
    return nal_type == 7 || nal_type == 8 || nal_type == 9;
}

// Parse 2-byte VVC NALU header. Returns false if too short.
static bool parseVvcNaluHeader(const uint8_t* data, int remaining,
                               uint8_t& nal_type, uint8_t& temporal_id,
                               uint8_t& layer_id) {
    if (remaining < 2) return false;
    uint8_t b0 = data[0], b1 = data[1];
    layer_id    = b0 & 0x3F;
    nal_type    = (b1 >> 3) & 0x1F;
    uint8_t tid_plus1 = b1 & 0x07;
    temporal_id = (tid_plus1 > 0) ? (tid_plus1 - 1) : 0;
    return true;
}

// Parse NALUs from Annex B data within a single packet.
// Appends entries to `nalu_entries` and advances `virtual_offset`.
static void parseAnnexBNalus(const uint8_t* data, int data_len,
                             std::vector<VbiEntry>& nalu_entries,
                             uint64_t& virtual_offset) {
    // Collect start code positions
    std::vector<int> start_positions;
    int i = 0;
    while (i < data_len - 3) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                start_positions.push_back(i);
                i += 3;
                continue;
            }
            if (i < data_len - 4 && data[i + 2] == 0 && data[i + 3] == 1) {
                start_positions.push_back(i);
                i += 4;
                continue;
            }
        }
        i++;
    }

    for (size_t idx = 0; idx < start_positions.size(); idx++) {
        int sc_pos = start_positions[idx];

        // Determine start code length
        int sc_len = 3;
        if (sc_pos + 4 <= data_len &&
            data[sc_pos] == 0 && data[sc_pos + 1] == 0 &&
            data[sc_pos + 2] == 0 && data[sc_pos + 3] == 1) {
            sc_len = 4;
        }

        // NALU size: from this start code to the next (or end of data)
        int nalu_size;
        if (idx + 1 < start_positions.size()) {
            nalu_size = start_positions[idx + 1] - sc_pos;
        } else {
            nalu_size = data_len - sc_pos;
        }

        // Parse VVC NALU header (2 bytes after start code)
        int hdr_offset = sc_pos + sc_len;
        uint8_t nal_type = 31, temporal_id = 0, layer_id = 0;
        if (hdr_offset + 2 <= data_len) {
            parseVvcNaluHeader(data + hdr_offset, data_len - hdr_offset,
                               nal_type, temporal_id, layer_id);
        }

        // Compute flags
        uint8_t flags = 0;
        if (isVclType(nal_type))      flags |= VBI_FLAG_IS_VCL;
        if (isSliceType(nal_type))    flags |= VBI_FLAG_IS_SLICE;
        if (isKeyframeType(nal_type)) flags |= VBI_FLAG_IS_KEYFRAME;

        VbiEntry entry{};
        entry.offset      = virtual_offset;
        entry.size        = static_cast<uint32_t>(nalu_size);
        entry.nal_type    = nal_type;
        entry.temporal_id = temporal_id;
        entry.layer_id    = layer_id;
        entry.flags       = flags;
        nalu_entries.push_back(entry);

        virtual_offset += static_cast<uint64_t>(nalu_size);
    }
}

// Parse NALUs from length-prefixed (MP4/MKV container) data within a single packet.
// Appends entries to `nalu_entries` and advances `virtual_offset`.
static void parseLengthPrefixedNalus(const uint8_t* data, int data_len,
                                     std::vector<VbiEntry>& nalu_entries,
                                     uint64_t& virtual_offset) {
    int pos = 0;
    while (pos + 4 < data_len) {
        // 4-byte big-endian NALU length
        uint32_t nalu_len = (static_cast<uint32_t>(data[pos]) << 24) |
                            (static_cast<uint32_t>(data[pos + 1]) << 16) |
                            (static_cast<uint32_t>(data[pos + 2]) << 8) |
                            static_cast<uint32_t>(data[pos + 3]);

        if (nalu_len == 0 || pos + 4 + static_cast<int>(nalu_len) > data_len) {
            break; // Invalid or truncated
        }

        const uint8_t* nalu_data = data + pos + 4;
        uint8_t nal_type = 31, temporal_id = 0, layer_id = 0;
        if (static_cast<int>(nalu_len) >= 2) {
            parseVvcNaluHeader(nalu_data, static_cast<int>(nalu_len),
                               nal_type, temporal_id, layer_id);
        }

        uint8_t flags = 0;
        if (isVclType(nal_type))      flags |= VBI_FLAG_IS_VCL;
        if (isSliceType(nal_type))    flags |= VBI_FLAG_IS_SLICE;
        if (isKeyframeType(nal_type)) flags |= VBI_FLAG_IS_KEYFRAME;

        // In virtual Annex B, each NALU has a 4-byte start code + nalu data
        uint32_t virtual_size = 4 + nalu_len;

        VbiEntry entry{};
        entry.offset      = virtual_offset;
        entry.size        = virtual_size;
        entry.nal_type    = nal_type;
        entry.temporal_id = temporal_id;
        entry.layer_id    = layer_id;
        entry.flags       = flags;
        nalu_entries.push_back(entry);

        virtual_offset += virtual_size;
        pos += 4 + static_cast<int>(nalu_len);
    }
}

// Detect if packet data is Annex B (starts with 00 00 01 or 00 00 00 01)
static bool isAnnexB(const uint8_t* data, int len) {
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
        return true;
    if (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        return true;
    return false;
}

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
                     const std::vector<VbiEntry>& entries,
                     uint32_t source_size) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    VbiHeader hdr{};
    hdr.magic[0] = 'V'; hdr.magic[1] = 'B';
    hdr.magic[2] = 'I'; hdr.magic[3] = '1';
    hdr.num_nalus   = static_cast<uint32_t>(entries.size());
    hdr.source_size = source_size;
    hdr.reserved    = 0;
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(VbiHeader));

    for (const auto& e : entries) {
        out.write(reinterpret_cast<const char*>(&e), sizeof(VbiEntry));
    }

    return out.good();
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
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        spdlog::error("[AnalysisGen] avformat_find_stream_info failed: {:#x}", static_cast<unsigned>(ret));
        avformat_close_input(&fmt_ctx);
        return false;
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
        return false;
    }

    AVRational time_base = fmt_ctx->streams[video_idx]->time_base;
    int32_t tb_num = time_base.num;
    int32_t tb_den = time_base.den;

    spdlog::info("[AnalysisGen] video stream {}: time_base={}/{}",
                 video_idx, tb_num, tb_den);

    // Single-pass: collect VBI and VBT data
    std::vector<VbiEntry> nalu_entries;
    std::vector<VbtEntry> pkt_entries;
    uint64_t virtual_offset = 0; // Virtual Annex B byte offset for VBI
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

        // --- VBI: parse NALUs from packet data ---
        if (pkt->data && pkt->size > 0) {
            if (isAnnexB(pkt->data, pkt->size)) {
                parseAnnexBNalus(pkt->data, pkt->size, nalu_entries, virtual_offset);
            } else {
                parseLengthPrefixedNalus(pkt->data, pkt->size, nalu_entries, virtual_offset);
            }
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);

    spdlog::info("[AnalysisGen] scanned {} packets, {} NALUs",
                 pkt_entries.size(), nalu_entries.size());

    // Write VBT
    bool vbt_ok = writeVbt(vbt_path, pkt_entries, tb_num, tb_den);
    if (!vbt_ok) {
        spdlog::error("[AnalysisGen] failed to write VBT: {}", vbt_path);
        return false;
    }
    spdlog::info("[AnalysisGen] wrote VBT: {} ({} entries)", vbt_path, pkt_entries.size());

    // Write VBI
    bool vbi_ok = writeVbi(vbi_path, nalu_entries, static_cast<uint32_t>(virtual_offset));
    if (!vbi_ok) {
        spdlog::warn("[AnalysisGen] failed to write VBI: {}", vbi_path);
        // VBI is optional — return true if VBT was written
    } else {
        spdlog::info("[AnalysisGen] wrote VBI: {} ({} entries)", vbi_path, nalu_entries.size());
    }

    return vbt_ok;
}

} // namespace vr::analysis

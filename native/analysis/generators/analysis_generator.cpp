#include "analysis/generators/analysis_generator.h"

#include "analysis/generators/bitstream_indexer.h"
#include "analysis/parsers/binary_types.h"
#include "analysis/parsers/vac2_parser.h"
#include "analysis/parsers/vachunk_parser.h"
#include "common/win_utf8.h"
#include "media/private_cdn_flv_demuxer.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
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

class MemoryUnitWriter {
public:
    explicit MemoryUnitWriter(OutputBudget* budget = nullptr) : budget_(budget) {}

    bool append(const AnalysisUnitScanEntry& entry) {
        if (entries_.size() == UINT32_MAX) return false;
        if (budget_ && !budget_->reserve(sizeof(AnalysisUnitScanEntry))) return false;
        entries_.push_back(entry);
        source_size_ = std::max(source_size_, entry.offset + entry.size);
        return true;
    }

    uint32_t count() const { return static_cast<uint32_t>(entries_.size()); }
    uint64_t source_size() const { return source_size_; }
    const std::vector<AnalysisUnitScanEntry>& entries() const { return entries_; }

private:
    std::vector<AnalysisUnitScanEntry> entries_;
    uint64_t source_size_ = 0;
    OutputBudget* budget_ = nullptr;
};

struct Vac2ScanData {
    AnalysisCodec codec = AnalysisCodec::Unknown;
    AnalysisUnitKind unit_kind = AnalysisUnitKind::Unknown;
    int32_t time_base_num = 1;
    int32_t time_base_den = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<AnalysisPacketScanEntry> packets;
    std::vector<AnalysisUnitScanEntry> units;
};

static uint64_t source_file_size(const std::string& path) {
    std::ifstream f(win_utf8::path_from_utf8(path), std::ios::binary | std::ios::ate);
    if (!f) return 0;
    const auto size = f.tellg();
    return size < 0 ? 0 : static_cast<uint64_t>(size);
}

static bool probe_video_geometry(const std::string& video_path,
                                 uint32_t& width,
                                 uint32_t& height) {
    width = 0;
    height = 0;

    FfmpegOpenTimeout timeout;
    AVFormatContext* fmt_ctx =
        alloc_format_context_with_timeout(timeout, std::chrono::seconds(10));
    if (!fmt_ctx) return false;

    if (avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr) < 0) {
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        return false;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        return false;
    }

    const int stream_index =
        av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0 ||
        stream_index >= static_cast<int>(fmt_ctx->nb_streams) ||
        !fmt_ctx->streams[stream_index] ||
        !fmt_ctx->streams[stream_index]->codecpar) {
        avformat_close_input(&fmt_ctx);
        return false;
    }

    const AVCodecParameters* params = fmt_ctx->streams[stream_index]->codecpar;
    if (params->width > 0 && params->height > 0) {
        width = static_cast<uint32_t>(params->width);
        height = static_cast<uint32_t>(params->height);
    }
    avformat_close_input(&fmt_ctx);
    return width > 0 && height > 0;
}

static uint8_t infer_slice_type(AnalysisCodec codec, uint8_t nal_type, uint8_t flags) {
    if ((flags & ANALYSIS_UNIT_FLAG_IS_KEYFRAME) != 0) {
        return 2;
    }
    if ((flags & ANALYSIS_UNIT_FLAG_IS_VCL) == 0) {
        return 255;
    }
    switch (codec) {
    case AnalysisCodec::H264:
        return nal_type == 5 ? 2 : 1;
    case AnalysisCodec::HEVC:
        return (nal_type >= 16 && nal_type <= 21) ? 2 : 1;
    case AnalysisCodec::VVC:
        return (nal_type >= 7 && nal_type <= 10) ? 2 : 1;
    case AnalysisCodec::AV1:
    case AnalysisCodec::VP9:
        return (flags & ANALYSIS_UNIT_FLAG_IS_KEYFRAME) != 0 ? 2 : 1;
    default:
        return 255;
    }
}

static bool supports_packet_order_reference_inference(AnalysisCodec codec) {
    return codec == AnalysisCodec::H264 ||
           codec == AnalysisCodec::HEVC ||
           codec == AnalysisCodec::VVC;
}

static int64_t packet_presentation_key(const AnalysisPacketScanEntry& packet,
                                       int fallback) {
    if (packet.pts != AV_NOPTS_VALUE) return packet.pts;
    if (packet.dts != AV_NOPTS_VALUE) return packet.dts;
    return fallback;
}

static std::vector<int32_t> infer_display_order_by_packet(
    const std::vector<AnalysisPacketScanEntry>& packets) {
    std::vector<size_t> order(packets.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&packets](size_t lhs, size_t rhs) {
        const auto& a = packets[lhs];
        const auto& b = packets[rhs];
        const int64_t a_pts = packet_presentation_key(a, static_cast<int>(lhs));
        const int64_t b_pts = packet_presentation_key(b, static_cast<int>(rhs));
        if (a_pts != b_pts) return a_pts < b_pts;
        if (a.dts != b.dts) return a.dts < b.dts;
        return lhs < rhs;
    });

    std::vector<int32_t> display_order(packets.size(), 0);
    for (size_t rank = 0; rank < order.size(); ++rank) {
        display_order[order[rank]] = static_cast<int32_t>(rank);
    }
    return display_order;
}

static int nearest_decoded_display_ref(const std::vector<int32_t>& display_order,
                                       int current,
                                       bool before_current_display) {
    const int32_t current_display = display_order[static_cast<size_t>(current)];
    int best = -1;
    int32_t best_display = before_current_display
        ? std::numeric_limits<int32_t>::min()
        : std::numeric_limits<int32_t>::max();
    for (int i = 0; i < current; ++i) {
        const int32_t candidate_display = display_order[static_cast<size_t>(i)];
        if (before_current_display) {
            if (candidate_display < current_display && candidate_display > best_display) {
                best = i;
                best_display = candidate_display;
            }
        } else if (candidate_display > current_display && candidate_display < best_display) {
            best = i;
            best_display = candidate_display;
        }
    }
    return best;
}

static void infer_packet_order_references(
    const std::vector<int32_t>& display_order,
    int frame_index,
    bool keyframe,
    Vac2FrameSummaryEntry& summary) {
    for (int i = 0; i < 15; ++i) {
        summary.ref_pocs_l0[i] = -1;
        summary.ref_pocs_l1[i] = -1;
    }

    if (keyframe || summary.slice_type == 2 || display_order.empty()) {
        summary.slice_type = 2;
        return;
    }

    const int previous_ref =
        nearest_decoded_display_ref(display_order, frame_index, true);
    const int future_ref =
        nearest_decoded_display_ref(display_order, frame_index, false);

    if (previous_ref >= 0 && future_ref >= 0) {
        summary.slice_type = 0;
        summary.num_ref_l0 = 1;
        summary.num_ref_l1 = 1;
        summary.ref_pocs_l0[0] = display_order[static_cast<size_t>(previous_ref)];
        summary.ref_pocs_l1[0] = display_order[static_cast<size_t>(future_ref)];
        summary.flags |= VAC2_FRAME_SUMMARY_FLAG_INFERRED_REFS;
        return;
    }

    summary.slice_type = 1;
    if (previous_ref >= 0) {
        summary.num_ref_l0 = 1;
        summary.ref_pocs_l0[0] = display_order[static_cast<size_t>(previous_ref)];
        summary.flags |= VAC2_FRAME_SUMMARY_FLAG_INFERRED_REFS;
    }
}

static bool build_vac2_base_from_scan(const std::string& video_path,
                                      const Vac2ScanData& scan,
                                      Vac2BaseData& out) {
    const int packet_count = static_cast<int>(scan.packets.size());
    const int unit_count = static_cast<int>(scan.units.size());
    if (packet_count <= 0 || unit_count < 0 || scan.codec == AnalysisCodec::Unknown) {
        return false;
    }

    out = {};
    out.codec = scan.codec;
    out.track_index = 0;
    out.time_base_num = scan.time_base_num;
    out.time_base_den = scan.time_base_den;
    out.width = scan.width;
    out.height = scan.height;
    out.source_size = source_file_size(video_path);
    out.content_revision = 3;
    if ((out.width == 0 || out.height == 0) &&
        !probe_video_geometry(video_path, out.width, out.height)) {
        spdlog::warn("[AnalysisGen] failed to probe VAC2 base dimensions: {}", video_path);
    }
    out.metadata_json =
        "{\"schema\":\"VAC2\",\"producer\":\"AnalysisGenerator::generate_vac2_base\","
        "\"source\":\"direct-vac2-scanner\","
        "\"frame_model\":\"one_packet_per_frame_fallback\","
        "\"reference_model\":\"packet_pts_reorder_inference\","
        "\"frame_model_warning\":\"packet_index_equals_frame_index\"}";

    out.packets.resize(static_cast<size_t>(packet_count));
    out.frames.resize(static_cast<size_t>(packet_count));
    out.frame_summaries.resize(static_cast<size_t>(packet_count));

    std::vector<uint32_t> first_unit_by_frame(static_cast<size_t>(packet_count), UINT32_MAX);
    std::vector<uint32_t> unit_count_by_frame(static_cast<size_t>(packet_count), 0);
    std::vector<uint32_t> first_vcl_by_frame(static_cast<size_t>(packet_count), UINT32_MAX);
    const std::vector<int32_t> display_order_by_frame =
        infer_display_order_by_packet(scan.packets);

    uint32_t vcl_seen = 0;
    out.units.reserve(scan.units.size());
    for (int i = 0; i < unit_count; ++i) {
        const auto& src = scan.units[static_cast<size_t>(i)];
        uint32_t frame_index = vcl_seen;
        if (frame_index >= static_cast<uint32_t>(packet_count)) {
            frame_index = static_cast<uint32_t>(packet_count - 1);
        }

        Vac2BitstreamUnitEntry unit{};
        unit.packet_index = frame_index;
        unit.au_index = frame_index;
        unit.offset = src.offset;
        unit.size = src.size;
        unit.payload_offset = 0;
        unit.nal_type = src.nal_type;
        unit.temporal_id = src.temporal_id;
        unit.layer_id = src.layer_id;
        unit.unit_kind = static_cast<uint8_t>(scan.unit_kind);
        unit.flags = 0;
        if ((src.flags & ANALYSIS_UNIT_FLAG_IS_VCL) != 0) unit.flags |= VAC2_UNIT_FLAG_IS_VCL;
        if ((src.flags & ANALYSIS_UNIT_FLAG_IS_SLICE) != 0) unit.flags |= VAC2_UNIT_FLAG_IS_SLICE;
        if ((src.flags & ANALYSIS_UNIT_FLAG_IS_KEYFRAME) != 0) unit.flags |= VAC2_UNIT_FLAG_IS_KEYFRAME;
        unit.pset_snapshot = UINT16_MAX;
        out.units.push_back(unit);

        auto& first_unit = first_unit_by_frame[frame_index];
        if (first_unit == UINT32_MAX) first_unit = static_cast<uint32_t>(i);
        ++unit_count_by_frame[frame_index];
        if ((src.flags & ANALYSIS_UNIT_FLAG_IS_VCL) != 0) {
            if (first_vcl_by_frame[frame_index] == UINT32_MAX) {
                first_vcl_by_frame[frame_index] = static_cast<uint32_t>(i);
            }
            ++vcl_seen;
        }
    }

    for (int i = 0; i < packet_count; ++i) {
        const auto& src = scan.packets[static_cast<size_t>(i)];
        const uint32_t index = static_cast<uint32_t>(i);
        Vac2PacketEntry packet{};
        packet.pts = src.pts;
        packet.dts = src.dts;
        packet.duration = src.duration;
        packet.size = src.size;
        packet.stream_index = 0;
        packet.flags = (src.flags & ANALYSIS_PACKET_FLAG_KEYFRAME) ? VAC2_PACKET_FLAG_KEYFRAME : 0;
        packet.file_offset = UINT64_MAX;
        packet.format_offset = UINT64_MAX;
        packet.first_unit = first_unit_by_frame[index] == UINT32_MAX ? 0 : first_unit_by_frame[index];
        packet.unit_count = unit_count_by_frame[index];
        packet.au_index = index;
        out.packets[static_cast<size_t>(i)] = packet;

        Vac2FrameEntry frame{};
        frame.first_packet = index;
        frame.packet_count = 1;
        frame.first_unit = packet.first_unit;
        frame.unit_count = packet.unit_count;
        frame.pts = src.pts;
        frame.dts = src.dts;
        frame.duration = src.duration;
        frame.coded_order = index;
        frame.display_order = display_order_by_frame[static_cast<size_t>(i)];
        frame.poc = frame.display_order;
        frame.frame_size = src.size;
        frame.rap_distance = 0;
        frame.flags = (src.flags & ANALYSIS_PACKET_FLAG_KEYFRAME)
            ? (VAC2_FRAME_FLAG_KEYFRAME | VAC2_FRAME_FLAG_RAP)
            : 0;
        frame.flags |= VAC2_FRAME_FLAG_INFERRED_AU;
        out.frames[static_cast<size_t>(i)] = frame;

        Vac2FrameSummaryEntry summary{};
        summary.poc = frame.poc;
        summary.coded_order = index;
        summary.first_vcl_unit = first_vcl_by_frame[index];
        summary.flags = VAC2_FRAME_SUMMARY_FLAG_INFERRED_AU;
        summary.slice_type = 255;
        summary.qp_kind = VAC2_QP_KIND_UNKNOWN;
        if (summary.first_vcl_unit != UINT32_MAX &&
            summary.first_vcl_unit < out.units.size()) {
            const auto& unit = out.units[summary.first_vcl_unit];
            summary.temporal_id = unit.temporal_id;
            summary.nal_type = unit.nal_type;
            summary.slice_type = infer_slice_type(out.codec, unit.nal_type, static_cast<uint8_t>(
                ((unit.flags & VAC2_UNIT_FLAG_IS_VCL) ? ANALYSIS_UNIT_FLAG_IS_VCL : 0) |
                ((unit.flags & VAC2_UNIT_FLAG_IS_SLICE) ? ANALYSIS_UNIT_FLAG_IS_SLICE : 0) |
                ((unit.flags & VAC2_UNIT_FLAG_IS_KEYFRAME) ? ANALYSIS_UNIT_FLAG_IS_KEYFRAME : 0)));
        }
        if (supports_packet_order_reference_inference(out.codec)) {
            infer_packet_order_references(
                display_order_by_frame,
                i,
                (frame.flags & VAC2_FRAME_FLAG_KEYFRAME) != 0,
                summary);
        }
        out.frame_summaries[static_cast<size_t>(i)] = summary;
    }

    return true;
}

static const char* annex_b_bsf_name(AnalysisCodec codec) {
    switch (codec) {
    case AnalysisCodec::H264: return "h264_mp4toannexb";
    case AnalysisCodec::HEVC: return "hevc_mp4toannexb";
    case AnalysisCodec::VVC:  return "vvc_mp4toannexb";
    default:             return nullptr;
    }
}

static bool requires_annex_b_filter(AnalysisCodec codec) {
    return codec == AnalysisCodec::H264 || codec == AnalysisCodec::HEVC || codec == AnalysisCodec::VVC;
}

static bool packet_starts_with_annex_b(const AVPacket* pkt) {
    if (!pkt || !pkt->data || pkt->size < 3) return false;
    const auto* data = pkt->data;
    const int len = pkt->size;
    return (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) ||
           (data[0] == 0 && data[1] == 0 && data[2] == 1);
}

template <typename UnitWriter>
static bool append_packet_if_safe(const AVPacket* pkt,
                                  AnalysisCodec codec,
                                  UnitWriter& writer,
                                  bool& unsafe_fallback_warned) {
    if (!pkt || !pkt->data || pkt->size <= 0) return true;

    if (requires_annex_b_filter(codec) && !packet_starts_with_annex_b(pkt)) {
        if (!unsafe_fallback_warned) {
            spdlog::warn(
                "[AnalysisGen] skipping length-prefixed packets without Annex-B BSF; VAC2 unit scan unavailable for this stream");
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
        [&writer](const AnalysisUnitScanEntry& entry) {
            return writer.append(entry);
        });
}

static AVBSFContext* create_annex_b_bsf(const AVCodecParameters* codecpar,
                                        AVRational time_base,
                                        AnalysisCodec codec) {
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

    spdlog::info("[AnalysisGen] using {} for VAC2 Annex-B indexing", name);
    return bsf;
}

static AVBSFContext* create_annex_b_bsf(AVStream* stream, AnalysisCodec codec) {
    if (!stream) return nullptr;
    return create_annex_b_bsf(stream->codecpar, stream->time_base, codec);
}

template <typename UnitWriter>
static bool append_filtered_packets(AVBSFContext* bsf,
                                    AVPacket* filtered_pkt,
                                    AnalysisCodec codec,
                                    UnitWriter& writer) {
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
                [&writer](const AnalysisUnitScanEntry& entry) {
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

static bool scanRawOnlyVac2(const std::string& video_path,
                            Vac2ScanData& out,
                            uint64_t max_output_bytes = 0) {
    AnalysisCodec codec = BitstreamIndexer::codec_from_path(video_path);
    if (codec == AnalysisCodec::Unknown) {
        return false;
    }

    OutputBudget budget(max_output_bytes);
    MemoryUnitWriter unit_writer(&budget);
    if (!budget.reserve(sizeof(Vac2Header))) return false;

    int32_t poc = 0;
    const bool indexed = BitstreamIndexer::index_raw_file_streaming(
        video_path,
        codec,
        [&](const AnalysisUnitScanEntry& unit) {
            if (!unit_writer.append(unit)) return false;
            if ((unit.flags & ANALYSIS_UNIT_FLAG_IS_VCL) == 0) return true;

            AnalysisPacketScanEntry entry{};
            entry.pts = poc;
            entry.dts = poc;
            entry.poc = poc++;
            entry.size = unit.size;
            entry.duration = 1;
            entry.flags = (unit.flags & ANALYSIS_UNIT_FLAG_IS_KEYFRAME) ? ANALYSIS_PACKET_FLAG_KEYFRAME : 0;
            if (!budget.reserve(sizeof(AnalysisPacketScanEntry))) return false;
            out.packets.push_back(entry);
            return true;
        });

    if (!indexed || out.packets.empty()) {
        return false;
    }

    out.codec = codec;
    out.unit_kind = BitstreamIndexer::unit_kind_for_codec(codec);
    out.time_base_num = 1;
    out.time_base_den = 60;
    out.units = unit_writer.entries();
    return true;
}

static bool scanPrivateCdnFlvVac2(const std::string& video_path,
                                  Vac2ScanData& out,
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
    const AnalysisCodec codec = BitstreamIndexer::codec_from_ffmpeg_id(
        stats.codec_params->codec_id);
    if (codec == AnalysisCodec::Unknown) {
        return false;
    }

    OutputBudget budget(max_output_bytes);
    if (!budget.reserve(sizeof(Vac2Header))) return false;
    MemoryUnitWriter unit_writer(&budget);

    AVPacket* pkt = av_packet_alloc();
    AVPacket* filtered_pkt = av_packet_alloc();
    if (!pkt || !filtered_pkt) {
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
            spdlog::warn("[AnalysisGen] private CDN FLV VAC2 read error: {:#x}",
                         static_cast<unsigned>(ret));
            break;
        }

        if (pkt->stream_index != stats.video_stream_index) {
            av_packet_unref(pkt);
            continue;
        }

        AnalysisPacketScanEntry entry{};
        entry.pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : 0;
        entry.dts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : 0;
        entry.poc = seq_poc++;
        entry.size = static_cast<uint32_t>(pkt->size);
        entry.duration = static_cast<uint32_t>(pkt->duration);
        entry.flags = (pkt->flags & AV_PKT_FLAG_KEY) ? ANALYSIS_PACKET_FLAG_KEYFRAME : 0;
        if (!budget.reserve(sizeof(AnalysisPacketScanEntry))) {
            scan_failed = true;
            av_packet_unref(pkt);
            break;
        }
        out.packets.push_back(entry);

        bool unit_ok = true;
        if (annex_b_bsf) {
            const int send_ret = av_bsf_send_packet(annex_b_bsf, pkt);
            if (send_ret >= 0) {
                unit_ok = append_filtered_packets(
                    annex_b_bsf, filtered_pkt, codec, unit_writer);
            } else {
                unit_ok = append_packet_if_safe(
                    pkt, codec, unit_writer, unsafe_fallback_warned);
            }
        } else {
            unit_ok = append_packet_if_safe(
                pkt, codec, unit_writer, unsafe_fallback_warned);
        }

        if (!unit_ok) {
            scan_failed = true;
            av_packet_unref(pkt);
            break;
        }
        av_packet_unref(pkt);
    }

    if (annex_b_bsf) {
        av_bsf_send_packet(annex_b_bsf, nullptr);
        if (!append_filtered_packets(annex_b_bsf, filtered_pkt, codec, unit_writer)) {
            scan_failed = true;
        }
    }

    if (annex_b_bsf) av_bsf_free(&annex_b_bsf);
    av_packet_free(&filtered_pkt);
    av_packet_free(&pkt);

    if (scan_failed || out.packets.empty()) {
        return false;
    }

    out.codec = codec;
    out.unit_kind = BitstreamIndexer::unit_kind_for_codec(codec);
    out.time_base_num = time_base.num;
    out.time_base_den = time_base.den;
    out.width = stats.codec_params->width > 0 ? static_cast<uint32_t>(stats.codec_params->width) : 0;
    out.height = stats.codec_params->height > 0 ? static_cast<uint32_t>(stats.codec_params->height) : 0;
    out.units = unit_writer.entries();
    return true;
}

static bool scanFfmpegVac2(const std::string& video_path,
                           Vac2ScanData& out,
                           uint64_t max_output_bytes) {
    FfmpegOpenTimeout timeout;
    AVFormatContext* fmt_ctx = alloc_format_context_with_timeout(timeout, std::chrono::seconds(30));
    if (!fmt_ctx) {
        spdlog::error("[AnalysisGen] failed to allocate format context");
        return scanRawOnlyVac2(video_path, out, max_output_bytes);
    }
    int ret = avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        timeout.deadline_ns = 0;
        spdlog::error("[AnalysisGen] avformat_open_input failed: {:#x}", static_cast<unsigned>(ret));
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        return scanRawOnlyVac2(video_path, out, max_output_bytes);
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    timeout.deadline_ns = 0;
    if (ret < 0) {
        spdlog::error("[AnalysisGen] avformat_find_stream_info failed: {:#x}", static_cast<unsigned>(ret));
        avformat_close_input(&fmt_ctx);
        return scanRawOnlyVac2(video_path, out, max_output_bytes);
    }

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
        return scanRawOnlyVac2(video_path, out, max_output_bytes);
    }

    AVStream* video_stream = fmt_ctx->streams[video_idx];
    AVRational time_base = video_stream->time_base;
    const int video_width = video_stream->codecpar->width;
    const int video_height = video_stream->codecpar->height;
    AnalysisCodec codec = BitstreamIndexer::codec_from_ffmpeg_id(
        video_stream->codecpar->codec_id);
    if (codec == AnalysisCodec::Unknown) {
        codec = BitstreamIndexer::codec_from_path(video_path);
    }

    OutputBudget budget(max_output_bytes);
    if (!budget.reserve(sizeof(Vac2Header))) {
        avformat_close_input(&fmt_ctx);
        return false;
    }
    MemoryUnitWriter unit_writer(&budget);

    bool unsafe_fallback_warned = false;
    AVBSFContext* annex_b_bsf = create_annex_b_bsf(video_stream, codec);
    AVPacket* pkt = av_packet_alloc();
    AVPacket* filtered_pkt = av_packet_alloc();
    if (!pkt || !filtered_pkt) {
        av_packet_free(&filtered_pkt);
        av_packet_free(&pkt);
        if (annex_b_bsf) av_bsf_free(&annex_b_bsf);
        avformat_close_input(&fmt_ctx);
        return false;
    }

    int32_t seq_poc = 0;
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

        AnalysisPacketScanEntry entry{};
        entry.pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : 0;
        entry.dts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : 0;
        entry.poc = seq_poc++;
        entry.size = static_cast<uint32_t>(pkt->size);
        entry.duration = static_cast<uint32_t>(pkt->duration);
        entry.flags = (pkt->flags & AV_PKT_FLAG_KEY) ? ANALYSIS_PACKET_FLAG_KEYFRAME : 0;
        if (!budget.reserve(sizeof(AnalysisPacketScanEntry))) {
            scan_failed = true;
            av_packet_unref(pkt);
            break;
        }
        out.packets.push_back(entry);

        bool unit_ok = true;
        if (annex_b_bsf) {
            const int send_ret = av_bsf_send_packet(annex_b_bsf, pkt);
            if (send_ret >= 0) {
                unit_ok = append_filtered_packets(
                    annex_b_bsf, filtered_pkt, codec, unit_writer);
            } else {
                spdlog::warn("[AnalysisGen] av_bsf_send_packet failed: {:#x}; skipping unsafe packet fallback if needed",
                             static_cast<unsigned>(send_ret));
                unit_ok = append_packet_if_safe(
                    pkt, codec, unit_writer, unsafe_fallback_warned);
            }
        } else {
            unit_ok = append_packet_if_safe(
                pkt, codec, unit_writer, unsafe_fallback_warned);
        }

        if (!unit_ok) {
            scan_failed = true;
            av_packet_unref(pkt);
            break;
        }

        av_packet_unref(pkt);
    }

    if (annex_b_bsf) {
        av_bsf_send_packet(annex_b_bsf, nullptr);
        if (!append_filtered_packets(annex_b_bsf, filtered_pkt, codec, unit_writer)) {
            scan_failed = true;
        }
    }

    av_packet_free(&filtered_pkt);
    av_packet_free(&pkt);
    if (annex_b_bsf) av_bsf_free(&annex_b_bsf);
    avformat_close_input(&fmt_ctx);

    if (scan_failed) return false;
    if (out.packets.empty()) {
        return scanRawOnlyVac2(video_path, out, max_output_bytes);
    }

    out.codec = codec;
    out.unit_kind = BitstreamIndexer::unit_kind_for_codec(codec);
    out.time_base_num = time_base.num;
    out.time_base_den = time_base.den;
    if (video_width > 0 && video_height > 0) {
        out.width = static_cast<uint32_t>(video_width);
        out.height = static_cast<uint32_t>(video_height);
    }
    out.units = unit_writer.entries();
    return true;
}

static bool copy_decoder_summary_into_base(Vac2BaseData& data,
                                           const std::vector<VachunkFrameSummary>& summaries) {
    if (summaries.empty() || data.frame_summaries.empty()) {
        return false;
    }

    size_t applied = 0;
    for (const auto& src : summaries) {
        const size_t frame_index = static_cast<size_t>(src.coded_order);
        if (frame_index >= data.frame_summaries.size()) {
            continue;
        }

        auto& dst = data.frame_summaries[frame_index];
        const uint32_t first_vcl_unit = dst.first_vcl_unit;
        const uint32_t au_flags = dst.flags & VAC2_FRAME_SUMMARY_FLAG_INFERRED_AU;
        dst.poc = src.poc;
        dst.coded_order = static_cast<uint32_t>(frame_index);
        dst.first_vcl_unit = first_vcl_unit;
        dst.flags = au_flags | VAC2_FRAME_SUMMARY_FLAG_EXACT_REFS;
        dst.temporal_id = src.temporal_id;
        dst.slice_type = src.slice_type;
        dst.nal_type = src.nal_unit_type;
        dst.qp_kind = VAC2_QP_KIND_UNKNOWN;
        dst.qp_avg = 0;
        dst.qp_min = 0;
        dst.qp_max = 0;
        dst.num_ref_l0 = std::min<uint8_t>(src.num_ref_l0, 15);
        dst.num_ref_l1 = std::min<uint8_t>(src.num_ref_l1, 15);
        for (int ref = 0; ref < 15; ++ref) {
            dst.ref_pocs_l0[ref] = ref < dst.num_ref_l0 ? src.ref_pocs_l0[ref] : -1;
            dst.ref_pocs_l1[ref] = ref < dst.num_ref_l1 ? src.ref_pocs_l1[ref] : -1;
        }
        if (frame_index < data.frames.size()) {
            data.frames[frame_index].poc = src.poc;
        }
        ++applied;
    }
    return applied > 0;
}

static const char* ffmpeg_analysis_codec_arg(AnalysisCodec codec) {
    switch (codec) {
    case AnalysisCodec::H264: return "h264";
    case AnalysisCodec::HEVC: return "hevc";
    case AnalysisCodec::VVC:  return "vvc";
    default:                  return nullptr;
    }
}

static bool file_exists_utf8(const std::string& path) {
    std::error_code ec;
    return !path.empty() &&
        std::filesystem::is_regular_file(win_utf8::path_from_utf8(path), ec) &&
        !ec;
}

static std::string find_ffmpeg_analyzer() {
    if (const char* env = std::getenv("VOID_FFMPEG_ANALYZER")) {
        if (file_exists_utf8(env)) return env;
    }

    std::vector<std::filesystem::path> roots;
    roots.push_back(win_utf8::path_from_utf8(win_utf8::module_directory_utf8()));
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec) roots.push_back(cwd);

    for (auto root : roots) {
        for (int depth = 0; depth < 8 && !root.empty(); ++depth) {
            const std::vector<std::filesystem::path> candidates = {
                root / L"tools" / L"ffmpeg-analysis" / L"void_ffmpeg_analyzer.exe",
                root / L"void_ffmpeg_analyzer.exe",
                root / L"analysis" / L"vendor" / L"ffmpeg" / L"bin" /
                    L"windows-x64" / L"void_ffmpeg_analyzer.exe",
                root / L"native" / L"analysis" / L"vendor" / L"ffmpeg" / L"bin" /
                    L"windows-x64" / L"void_ffmpeg_analyzer.exe",
            };
            for (const auto& candidate : candidates) {
                const auto text = win_utf8::path_to_utf8(candidate);
                if (file_exists_utf8(text)) return text;
            }
            if (!root.has_parent_path() || root.parent_path() == root) break;
            root = root.parent_path();
        }
    }
    return {};
}

static bool run_hidden_command(const std::string& command) {
#ifdef _WIN32
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::wstring cmdline = win_utf8::utf16_from_utf8(command);
    if (cmdline.empty()) return false;
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
#else
    return std::system(command.c_str()) == 0;
#endif
}

static std::string make_temp_summary_path() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = std::filesystem::current_path(ec);
#ifdef _WIN32
    const auto name = L"voidplayer_vac2_summary." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64()) + L".vck";
#else
    const auto name = "voidplayer_vac2_summary.vck";
#endif
    return win_utf8::path_to_utf8(dir / name);
}

static bool read_frame_summary_chunk(const std::string& path,
                                     std::vector<VachunkFrameSummary>& out) {
    out.clear();
    VachunkFile chunk;
    if (!chunk.open(path)) return false;
    const auto& header = chunk.header();
    if (header.kind != static_cast<uint16_t>(VachunkKind::FrameSummaryExact)) {
        return false;
    }
    const auto* section = chunk.section("FSUM");
    if (!section ||
        section->entry_size != sizeof(VachunkFrameSummary) ||
        section->decoded_size !=
            static_cast<uint64_t>(section->entry_count) * sizeof(VachunkFrameSummary)) {
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!chunk.read_section("FSUM", bytes) ||
        bytes.size() != section->decoded_size) {
        return false;
    }
    out.resize(section->entry_count);
    if (!bytes.empty()) {
        std::memcpy(out.data(), bytes.data(), bytes.size());
    }
    return !out.empty();
}

static bool collect_decoder_frame_summaries(const std::string& video_path,
                                            Vac2BaseData& data) {
    if (data.codec != AnalysisCodec::H264 &&
        data.codec != AnalysisCodec::HEVC &&
        data.codec != AnalysisCodec::VVC) {
        return false;
    }

    const char* codec_arg = ffmpeg_analysis_codec_arg(data.codec);
    if (!codec_arg) return false;

    const std::string analyzer = find_ffmpeg_analyzer();
    if (analyzer.empty()) {
        spdlog::warn("[AnalysisGen] FFmpeg analyzer not found for decoder frame summary");
        return false;
    }

    const std::string summary_path = make_temp_summary_path();
    win_utf8::delete_file_utf8(summary_path);
    const std::string cmd = "\"" + analyzer +
        "\" --codec " + codec_arg +
        " --input \"" + video_path +
        "\" --frame-summary \"" + summary_path + "\"";

    spdlog::debug("[AnalysisGen] decoder frame summary cmd: {}", cmd);
    if (!run_hidden_command(cmd) || !file_exists_utf8(summary_path)) {
        win_utf8::delete_file_utf8(summary_path);
        return false;
    }

    std::vector<VachunkFrameSummary> summaries;
    const bool read = read_frame_summary_chunk(summary_path, summaries);
    win_utf8::delete_file_utf8(summary_path);
    if (!read) return false;

    const bool ok = copy_decoder_summary_into_base(data, summaries);
    if (ok) {
        data.metadata_json =
            "{\"schema\":\"VAC2\",\"producer\":\"AnalysisGenerator::generate_vac2_base\","
            "\"source\":\"direct-vac2-scanner+ffmpeg-analysis-frame-summary\","
            "\"frame_model\":\"one_packet_per_frame_fallback\","
            "\"reference_model\":\"decoder_frame_summary\","
            "\"frame_model_warning\":\"packet_index_equals_frame_index\"}";
    }
    return ok;
}

// ---------------------------------------------------------------
// Public API
// ---------------------------------------------------------------

bool AnalysisGenerator::generate_vac2_base(const std::string& video_path,
                                           const std::string& vac2_path,
                                           uint64_t max_output_bytes) {
    if (video_path.empty() || vac2_path.empty()) return false;

    const std::filesystem::path out_path = win_utf8::path_from_utf8(vac2_path);
    const std::filesystem::path parent = out_path.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) return false;
    }

    Vac2ScanData scan;
    const bool scanned = vr::PrivateCdnFlvDemuxer::probe(video_path)
        ? scanPrivateCdnFlvVac2(video_path, scan, max_output_bytes) ||
              scanFfmpegVac2(video_path, scan, max_output_bytes)
        : scanFfmpegVac2(video_path, scan, max_output_bytes);
    if (!scanned) return false;

    Vac2BaseData data;
    if (!build_vac2_base_from_scan(video_path, scan, data)) {
        return false;
    }
    if (!collect_decoder_frame_summaries(video_path, data)) {
        spdlog::warn("[AnalysisGen] decoder frame summary unavailable; keeping inferred VAC2 refs");
    }

    return write_vac2_base_container(vac2_path, data, max_output_bytes);
}

} // namespace vr::analysis

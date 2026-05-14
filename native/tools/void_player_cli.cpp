#include "analysis/cache/overlay_chunk.h"
#include "analysis/cache/overlay_raster.h"
#include "analysis/cache/vacache_store.h"
#include "analysis/generators/analysis_generator.h"
#include "analysis/parsers/binary_types.h"
#include "analysis/parsers/vac2_parser.h"
#include "analysis/parsers/vachunk_parser.h"
#include "common/win_utf8.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

namespace {

constexpr uint64_t kOverlayVachunkFeatureFlags =
    VACHUNK_FEATURE_CU_GEOMETRY |
    VACHUNK_FEATURE_QP |
    VACHUNK_FEATURE_PRED_MODE |
    VACHUNK_FEATURE_MOTION_VECTORS |
    VACHUNK_FEATURE_REF_INDEXES |
    VACHUNK_FEATURE_BIT_COST;

enum class CacheKind {
    Unknown,
    Vac2,
    Vachunk,
};

struct CliOptions {
    std::string command;
    std::string path;
    std::string value;
    bool json = false;
    uint32_t frame = UINT32_MAX;
    uint32_t index = UINT32_MAX;
    uint32_t limit = 5;
    uint32_t start_frame = UINT32_MAX;
    uint32_t end_frame = UINT32_MAX;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t iterations = 120;
    uint64_t generator_revision = 2;
    uint64_t max_cache_bytes = 0;
    std::string input;
    std::string cache_root;
    std::string hash;
    std::string analyzer;
    std::string codec;
    std::string mode = "bitrate";
    bool with_grid = false;
};

std::string fourcc(const char value[4]) {
    return std::string(value, value + 4);
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u"
                    << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch))
                    << std::dec << std::setfill(' ');
            } else {
                out << ch;
            }
            break;
        }
    }
    return out.str();
}

std::string codec_name(uint16_t codec) {
    switch (static_cast<AnalysisCodec>(codec)) {
    case AnalysisCodec::H264: return "h264";
    case AnalysisCodec::HEVC: return "hevc";
    case AnalysisCodec::VVC: return "vvc";
    case AnalysisCodec::AV1: return "av1";
    case AnalysisCodec::VP9: return "vp9";
    case AnalysisCodec::MPEG2: return "mpeg2";
    case AnalysisCodec::Unknown:
    default:
        return "unknown";
    }
}

std::string vachunk_kind_name(uint16_t kind) {
    switch (static_cast<VachunkKind>(kind)) {
    case VachunkKind::NaluDetail: return "naluDetail";
    case VachunkKind::FrameSummaryExact: return "frameSummaryExact";
    case VachunkKind::Overlay: return "overlay";
    case VachunkKind::HitTest: return "hitTest";
    case VachunkKind::Export: return "export";
    case VachunkKind::Unknown:
    default:
        return "unknown";
    }
}

std::string compression_name(uint16_t compression) {
    switch (compression) {
    case VACHUNK_COMPRESSION_NONE: return "none";
    case VACHUNK_COMPRESSION_ZSTD: return "zstd";
    default: return "unknown";
    }
}

std::string slice_type_name(uint8_t value) {
    switch (value) {
    case 0: return "B";
    case 1: return "P";
    case 2: return "I";
    case 255: return "unknown";
    default: return std::to_string(value);
    }
}

std::string qp_kind_name(uint8_t value) {
    switch (value) {
    case VAC2_QP_KIND_UNKNOWN: return "unknown";
    case VAC2_QP_KIND_SLICE: return "slice";
    case VAC2_QP_KIND_BASE: return "base";
    case VAC2_QP_KIND_ESTIMATED: return "estimated";
    case VAC2_QP_KIND_EXACT: return "exact";
    default: return std::to_string(value);
    }
}

std::vector<std::string> feature_names(uint64_t features) {
    std::vector<std::string> names;
    if ((features & VACHUNK_FEATURE_CU_GEOMETRY) != 0) names.push_back("cuGeometry");
    if ((features & VACHUNK_FEATURE_QP) != 0) names.push_back("qp");
    if ((features & VACHUNK_FEATURE_PRED_MODE) != 0) names.push_back("predMode");
    if ((features & VACHUNK_FEATURE_MOTION_VECTORS) != 0) names.push_back("motionVectors");
    if ((features & VACHUNK_FEATURE_REF_INDEXES) != 0) names.push_back("refIndexes");
    if ((features & VACHUNK_FEATURE_PU_GEOMETRY) != 0) names.push_back("puGeometry");
    if ((features & VACHUNK_FEATURE_TU_GEOMETRY) != 0) names.push_back("tuGeometry");
    if ((features & VACHUNK_FEATURE_CODEC_TOOLS) != 0) names.push_back("codecTools");
    if ((features & VACHUNK_FEATURE_BIT_COST) != 0) names.push_back("bitCost");
    return names;
}

bool parse_u32(const std::string& text, uint32_t& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (!end || *end != '\0' || value > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

bool parse_u64(const std::string& text, uint64_t& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = static_cast<uint64_t>(value);
    return true;
}

AnalysisCodec parse_codec(const std::string& text) {
    if (text == "h264" || text == "avc") return AnalysisCodec::H264;
    if (text == "hevc" || text == "h265") return AnalysisCodec::HEVC;
    if (text == "vvc" || text == "h266") return AnalysisCodec::VVC;
    if (text == "av1") return AnalysisCodec::AV1;
    if (text == "vp9") return AnalysisCodec::VP9;
    if (text == "mpeg2") return AnalysisCodec::MPEG2;
    return AnalysisCodec::Unknown;
}

const char* ffmpeg_analysis_codec_arg(AnalysisCodec codec) {
    switch (codec) {
    case AnalysisCodec::VVC: return "vvc";
    case AnalysisCodec::HEVC: return "hevc";
    case AnalysisCodec::H264: return "h264";
    default: return nullptr;
    }
}

CacheKind detect_kind(const std::string& path) {
    std::ifstream file(vr::win_utf8::path_from_utf8(path), std::ios::binary);
    if (!file) return CacheKind::Unknown;
    char magic[4] = {};
    file.read(magic, sizeof(magic));
    if (std::memcmp(magic, "VAC2", 4) == 0) return CacheKind::Vac2;
    if (std::memcmp(magic, "VCK1", 4) == 0) return CacheKind::Vachunk;
    return CacheKind::Unknown;
}

void print_usage(std::ostream& out) {
    out <<
        "VoidPlayerCli - inspect VoidPlayer VAC2/VACHUNK cache files\n\n"
        "Usage:\n"
        "  VoidPlayerCli inspect <base.vac|chunk.vck> [--json] [--limit N]\n"
        "  VoidPlayerCli check <base.vac|chunk.vck> [--json]\n"
        "  VoidPlayerCli frame <base.vac> --index N [--json]\n"
        "  VoidPlayerCli chunk-frame <chunk.vck> --frame N [--json] [--limit N]\n"
        "  VoidPlayerCli benchmark-overlay <chunk.vck> --frame N [--width N] [--height N] [--iterations N] [--mode bitrate|qp] [--with-grid] [--json]\n\n"
        "  VoidPlayerCli generate-base --input <video> --cache-root <dir> --hash <hash> [--json]\n"
        "  VoidPlayerCli generate-overlay --input <video> --cache-root <dir> --hash <hash> --start-frame N --end-frame N [--codec h264|hevc|vvc] [--analyzer <exe>] [--json]\n\n"
        "Examples:\n"
        "  VoidPlayerCli inspect \"%APPDATA%\\VoidPlayer\\cache\\<hash>\\base.vac\"\n"
        "  VoidPlayerCli chunk-frame \"overlay.vck\" --frame 128 --json\n"
        "  VoidPlayerCli benchmark-overlay \"overlay.vck\" --frame 0 --iterations 240 --with-grid --json\n"
        "  VoidPlayerCli generate-base --input input.mp4 --cache-root \"%APPDATA%\\VoidPlayer\\cache\" --hash <hash>\n"
        "  VoidPlayerCli generate-overlay --input input.mp4 --cache-root \"%APPDATA%\\VoidPlayer\\cache\" --hash <hash> --start-frame 128 --end-frame 191\n";
}

bool parse_args(const std::vector<std::string>& args, CliOptions& options) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        options.command = "help";
        return true;
    }
    options.command = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--json") {
            options.json = true;
        } else if (arg == "--with-grid") {
            options.with_grid = true;
        } else if (arg == "--limit" && i + 1 < args.size()) {
            if (!parse_u32(args[++i], options.limit)) return false;
        } else if (arg == "--index" && i + 1 < args.size()) {
            if (!parse_u32(args[++i], options.index)) return false;
        } else if (arg == "--frame" && i + 1 < args.size()) {
            if (!parse_u32(args[++i], options.frame)) return false;
        } else if (arg == "--start-frame" && i + 1 < args.size()) {
            if (!parse_u32(args[++i], options.start_frame)) return false;
        } else if (arg == "--end-frame" && i + 1 < args.size()) {
            if (!parse_u32(args[++i], options.end_frame)) return false;
        } else if (arg == "--width" && i + 1 < args.size()) {
            if (!parse_u32(args[++i], options.width)) return false;
        } else if (arg == "--height" && i + 1 < args.size()) {
            if (!parse_u32(args[++i], options.height)) return false;
        } else if (arg == "--iterations" && i + 1 < args.size()) {
            if (!parse_u32(args[++i], options.iterations)) return false;
        } else if (arg == "--generator-revision" && i + 1 < args.size()) {
            if (!parse_u64(args[++i], options.generator_revision)) return false;
        } else if (arg == "--max-cache-bytes" && i + 1 < args.size()) {
            if (!parse_u64(args[++i], options.max_cache_bytes)) return false;
        } else if (arg == "--input" && i + 1 < args.size()) {
            options.input = args[++i];
        } else if (arg == "--cache-root" && i + 1 < args.size()) {
            options.cache_root = args[++i];
        } else if (arg == "--hash" && i + 1 < args.size()) {
            options.hash = args[++i];
        } else if (arg == "--analyzer" && i + 1 < args.size()) {
            options.analyzer = args[++i];
        } else if (arg == "--codec" && i + 1 < args.size()) {
            options.codec = args[++i];
        } else if (arg == "--mode" && i + 1 < args.size()) {
            options.mode = args[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            return false;
        } else if (options.path.empty()) {
            options.path = arg;
        } else if (options.value.empty()) {
            options.value = arg;
        } else {
            return false;
        }
    }

    if (options.command == "frame" && options.index == UINT32_MAX &&
        !options.value.empty()) {
        if (!parse_u32(options.value, options.index)) return false;
    }
    if (options.command == "chunk-frame" && options.frame == UINT32_MAX &&
        !options.value.empty()) {
        if (!parse_u32(options.value, options.frame)) return false;
    }
    if (options.command == "benchmark-overlay" && options.frame == UINT32_MAX &&
        !options.value.empty()) {
        if (!parse_u32(options.value, options.frame)) return false;
    }
    return true;
}

bool path_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(vr::win_utf8::path_from_utf8(path), ec);
}

bool create_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(vr::win_utf8::path_from_utf8(path), ec);
    return !ec;
}

bool replace_file(const std::string& from, const std::string& to) {
    std::error_code ec;
    std::filesystem::remove(vr::win_utf8::path_from_utf8(to), ec);
    ec.clear();
    std::filesystem::rename(vr::win_utf8::path_from_utf8(from),
                            vr::win_utf8::path_from_utf8(to),
                            ec);
    return !ec;
}

std::string join_path(const std::string& lhs, const std::string& rhs) {
    return vr::win_utf8::path_to_utf8(
        vr::win_utf8::path_from_utf8(lhs) / vr::win_utf8::path_from_utf8(rhs));
}

std::string temp_name(const std::string& prefix, const std::string& suffix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ticks =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return prefix + "." + std::to_string(ticks) + suffix;
}

std::string find_analyzer(const std::string& explicit_path) {
    if (!explicit_path.empty() && path_exists(explicit_path)) {
        return explicit_path;
    }
    const std::string env = vr::win_utf8::get_env_utf8(L"VOID_FFMPEG_ANALYZER");
    if (!env.empty() && path_exists(env)) {
        return env;
    }
    const std::string exe_dir = vr::win_utf8::module_directory_utf8();
    const std::string candidates[] = {
        join_path(exe_dir, "tools\\ffmpeg-analysis\\void_ffmpeg_analyzer.exe"),
        join_path(exe_dir, "tools\\ffmpeg-analysis\\void_hevc_analyzer.exe"),
        join_path(exe_dir, "void_ffmpeg_analyzer.exe"),
    };
    for (const auto& candidate : candidates) {
        if (path_exists(candidate)) return candidate;
    }
    return {};
}

int run_process(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
#ifdef _WIN32
    std::vector<std::wstring> storage;
    storage.reserve(args.size());
    for (const auto& arg : args) {
        storage.push_back(vr::win_utf8::utf16_from_utf8(arg));
    }
    std::vector<const wchar_t*> argv;
    argv.reserve(storage.size() + 1);
    for (const auto& arg : storage) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);
    return static_cast<int>(_wspawnv(_P_WAIT, storage[0].c_str(), argv.data()));
#else
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);
    return static_cast<int>(spawnv(P_WAIT, args[0].c_str(), argv.data()));
#endif
}

template <typename T>
bool read_vachunk_records(const vr::analysis::VachunkFile& chunk,
                          const char (&type)[5],
                          std::vector<T>& out) {
    out.clear();
    const auto* section = chunk.section(type);
    if (!section ||
        section->entry_size != sizeof(T) ||
        section->decoded_size != static_cast<uint64_t>(section->entry_count) * sizeof(T)) {
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!chunk.read_section(type, bytes) || bytes.size() != section->decoded_size) {
        return false;
    }
    out.resize(section->entry_count);
    if (!out.empty()) {
        std::memcpy(out.data(), bytes.data(), bytes.size());
    }
    return true;
}

bool validate_vac2(const vr::analysis::Vac2BaseFile& vac2, std::string& error) {
    const auto& packets = vac2.packets();
    const auto& units = vac2.units();
    const auto& frames = vac2.frames();
    for (size_t i = 0; i < packets.size(); ++i) {
        const auto& packet = packets[i];
        if (packet.first_unit > units.size() ||
            packet.unit_count > units.size() - packet.first_unit) {
            error = "packet unit range out of bounds at packet " + std::to_string(i);
            return false;
        }
        if (packet.au_index != UINT32_MAX && packet.au_index >= frames.size()) {
            error = "packet au_index out of bounds at packet " + std::to_string(i);
            return false;
        }
    }
    for (size_t i = 0; i < units.size(); ++i) {
        const auto& unit = units[i];
        if (unit.packet_index != UINT32_MAX && unit.packet_index >= packets.size()) {
            error = "unit packet_index out of bounds at unit " + std::to_string(i);
            return false;
        }
        if (unit.au_index != UINT32_MAX && unit.au_index >= frames.size()) {
            error = "unit au_index out of bounds at unit " + std::to_string(i);
            return false;
        }
    }
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& frame = frames[i];
        if (frame.first_packet > packets.size() ||
            frame.packet_count > packets.size() - frame.first_packet) {
            error = "frame packet range out of bounds at frame " + std::to_string(i);
            return false;
        }
        if (frame.first_unit > units.size() ||
            frame.unit_count > units.size() - frame.first_unit) {
            error = "frame unit range out of bounds at frame " + std::to_string(i);
            return false;
        }
    }
    return true;
}

bool validate_vachunk(const vr::analysis::VachunkFile& chunk, std::string& error) {
    const auto& header = chunk.header();
    if (header.kind != static_cast<uint16_t>(VachunkKind::Overlay)) {
        return true;
    }

    std::vector<VachunkFrameSummary> summaries;
    std::vector<VachunkOverlayFrameIndexEntry> index;
    std::vector<vr::analysis::VachunkCuRecord> records;
    if (!read_vachunk_records(chunk, "FSUM", summaries) ||
        !read_vachunk_records(chunk, "FIDX", index) ||
        !read_vachunk_records(chunk, "CU4R", records)) {
        error = "overlay VACHUNK is missing FSUM/FIDX/CU4R or has invalid section layout";
        return false;
    }
    if (summaries.size() != index.size()) {
        error = "FSUM and FIDX entry counts differ";
        return false;
    }
    const uint64_t expected_frames =
        static_cast<uint64_t>(header.end_frame) - header.start_frame + 1;
    if (expected_frames != index.size()) {
        error = "FIDX entry count does not match header frame range";
        return false;
    }
    for (size_t i = 0; i < index.size(); ++i) {
        const auto& entry = index[i];
        if (entry.frame_index != header.start_frame + i) {
            error = "FIDX frame_index is not contiguous at local frame " +
                    std::to_string(i);
            return false;
        }
        if (entry.first_unit > records.size() ||
            entry.unit_count > records.size() - entry.first_unit) {
            error = "FIDX CU range out of bounds at frame " +
                    std::to_string(entry.frame_index);
            return false;
        }
    }
    return true;
}

bool vachunk_matches_key(const vr::analysis::VachunkFile& chunk,
                         const vr::analysis::VachunkKey& key) {
    const auto& h = chunk.header();
    return h.kind == static_cast<uint16_t>(key.kind) &&
           h.codec == static_cast<uint16_t>(key.codec) &&
           h.feature_flags == key.feature_flags &&
           h.base_content_revision == key.base_content_revision &&
           h.generator_revision == key.generator_revision &&
           h.start_frame == key.start_frame &&
           h.end_frame == key.end_frame &&
           h.start_packet == key.start_packet &&
           h.end_packet == key.end_packet &&
           h.start_unit == key.start_unit &&
           h.end_unit == key.end_unit;
}

int inspect_vac2(const std::string& path, bool json, uint32_t limit) {
    vr::analysis::Vac2BaseFile vac2;
    if (!vac2.open(path)) {
        std::cerr << "Failed to open VAC2: " << path << "\n";
        return 2;
    }
    const auto& h = vac2.header();
    if (json) {
        std::cout
            << "{"
            << "\"type\":\"vac2\","
            << "\"path\":\"" << json_escape(path) << "\","
            << "\"codec\":\"" << codec_name(h.codec) << "\","
            << "\"version\":\"" << h.version_major << "." << h.version_minor << "\","
            << "\"trackIndex\":" << h.track_index << ","
            << "\"sizeBytes\":" << h.file_size << ","
            << "\"sourceSizeBytes\":" << h.source_size << ","
            << "\"contentRevision\":" << h.content_revision << ","
            << "\"timeBase\":{\"num\":" << h.time_base_num
            << ",\"den\":" << h.time_base_den << "},"
            << "\"dimensions\":{\"width\":" << h.width
            << ",\"height\":" << h.height << "},"
            << "\"counts\":{\"packets\":" << h.packet_count
            << ",\"units\":" << h.unit_count
            << ",\"frames\":" << h.au_count << "},"
            << "\"sections\":[";
        const char* names[] = {"META", "PKT2", "BSU2", "AUF2", "FSUM"};
        for (size_t i = 0; i < 5; ++i) {
            const auto* section = vac2.section(names[i]);
            if (i != 0) std::cout << ",";
            std::cout << "{"
                      << "\"type\":\"" << names[i] << "\","
                      << "\"present\":" << (section ? "true" : "false");
            if (section) {
                std::cout << ",\"offset\":" << section->offset
                          << ",\"size\":" << section->size
                          << ",\"entrySize\":" << section->entry_size
                          << ",\"entryCount\":" << section->entry_count;
            }
            std::cout << "}";
        }
        std::cout << "],\"frames\":[";
        const uint32_t count = std::min<uint32_t>(limit, h.au_count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& frame = vac2.frames()[i];
            const auto& summary = vac2.frame_summaries()[i];
            if (i != 0) std::cout << ",";
            std::cout << "{"
                      << "\"index\":" << i
                      << ",\"pts\":" << frame.pts
                      << ",\"dts\":" << frame.dts
                      << ",\"duration\":" << frame.duration
                      << ",\"poc\":" << frame.poc
                      << ",\"codedOrder\":" << frame.coded_order
                      << ",\"displayOrder\":" << frame.display_order
                      << ",\"frameSize\":" << frame.frame_size
                      << ",\"keyframe\":" << ((frame.flags & VAC2_FRAME_FLAG_KEYFRAME) ? "true" : "false")
                      << ",\"sliceType\":\"" << slice_type_name(summary.slice_type) << "\""
                      << ",\"qpKind\":\"" << qp_kind_name(summary.qp_kind) << "\""
                      << ",\"qpAvg\":" << static_cast<int>(summary.qp_avg)
                      << ",\"qpMin\":" << static_cast<int>(summary.qp_min)
                      << ",\"qpMax\":" << static_cast<int>(summary.qp_max)
                      << "}";
        }
        std::cout << "]}\n";
        return 0;
    }

    std::cout << "VAC2 " << codec_name(h.codec) << " v"
              << h.version_major << "." << h.version_minor << "\n"
              << "path: " << path << "\n"
              << "track: " << h.track_index
              << ", size: " << h.file_size
              << ", source: " << h.source_size
              << ", revision: " << h.content_revision << "\n"
              << "video: " << h.width << "x" << h.height
              << ", time_base: " << h.time_base_num << "/" << h.time_base_den << "\n"
              << "counts: packets=" << h.packet_count
              << ", units=" << h.unit_count
              << ", frames=" << h.au_count << "\n";
    std::cout << "sections:";
    const char* names[] = {"META", "PKT2", "BSU2", "AUF2", "FSUM"};
    for (const char* name : names) {
        const auto* section = vac2.section(name);
        if (!section) {
            std::cout << " " << name << "=missing";
        } else {
            std::cout << " " << name << "=" << section->entry_count
                      << "x" << section->entry_size;
        }
    }
    std::cout << "\n";

    const uint32_t count = std::min<uint32_t>(limit, h.au_count);
    if (count > 0) {
        std::cout << "frames[0.." << (count - 1) << "]:\n";
    }
    for (uint32_t i = 0; i < count; ++i) {
        const auto& frame = vac2.frames()[i];
        const auto& summary = vac2.frame_summaries()[i];
        std::cout << "  #" << i
                  << " pts=" << frame.pts
                  << " dts=" << frame.dts
                  << " dur=" << frame.duration
                  << " poc=" << frame.poc
                  << " size=" << frame.frame_size
                  << " slice=" << slice_type_name(summary.slice_type)
                  << " qp=" << static_cast<int>(summary.qp_avg)
                  << " (" << qp_kind_name(summary.qp_kind) << ")"
                  << ((frame.flags & VAC2_FRAME_FLAG_KEYFRAME) ? " key" : "")
                  << "\n";
    }
    return 0;
}

int inspect_vachunk(const std::string& path, bool json, uint32_t limit) {
    vr::analysis::VachunkFile chunk;
    if (!chunk.open(path)) {
        std::cerr << "Failed to open VACHUNK: " << path << "\n";
        return 2;
    }
    const auto& h = chunk.header();
    const auto features = feature_names(h.feature_flags);
    if (json) {
        std::cout
            << "{"
            << "\"type\":\"vachunk\","
            << "\"path\":\"" << json_escape(path) << "\","
            << "\"kind\":\"" << vachunk_kind_name(h.kind) << "\","
            << "\"codec\":\"" << codec_name(h.codec) << "\","
            << "\"version\":\"" << h.version_major << "." << h.version_minor << "\","
            << "\"trackIndex\":" << h.track_index << ","
            << "\"compression\":\"" << compression_name(h.compression) << "\","
            << "\"sizeBytes\":" << h.file_size << ","
            << "\"baseRevision\":" << h.base_content_revision << ","
            << "\"generatorRevision\":" << h.generator_revision << ","
            << "\"frameRange\":{\"start\":" << h.start_frame
            << ",\"end\":" << h.end_frame << "},"
            << "\"packetRange\":{\"start\":" << h.start_packet
            << ",\"end\":" << h.end_packet << "},"
            << "\"unitRange\":{\"start\":" << h.start_unit
            << ",\"end\":" << h.end_unit << "},"
            << "\"features\":[";
        for (size_t i = 0; i < features.size(); ++i) {
            if (i != 0) std::cout << ",";
            std::cout << "\"" << features[i] << "\"";
        }
        std::cout << "],\"sections\":[";
        const char* names[] = {"FSUM", "FIDX", "CU4R", "META"};
        bool first = true;
        for (const char* name : names) {
            const auto* section = chunk.section(name);
            if (!section) continue;
            if (!first) std::cout << ",";
            first = false;
            std::cout << "{"
                      << "\"type\":\"" << name << "\","
                      << "\"offset\":" << section->offset
                      << ",\"size\":" << section->size
                      << ",\"decodedSize\":" << section->decoded_size
                      << ",\"compressed\":" << (((section->flags & VACHUNK_SECTION_FLAG_ZSTD) != 0) ? "true" : "false")
                      << ",\"entrySize\":" << section->entry_size
                      << ",\"entryCount\":" << section->entry_count
                      << "}";
        }
        std::cout << "]";
        if (h.kind == static_cast<uint16_t>(VachunkKind::Overlay)) {
            std::vector<VachunkFrameSummary> summaries;
            std::vector<VachunkOverlayFrameIndexEntry> index;
            std::vector<vr::analysis::VachunkCuRecord> records;
            if (read_vachunk_records(chunk, "FSUM", summaries) &&
                read_vachunk_records(chunk, "FIDX", index) &&
                read_vachunk_records(chunk, "CU4R", records)) {
                std::cout << ",\"overlay\":{\"frames\":" << index.size()
                          << ",\"cus\":" << records.size()
                          << ",\"sampleFrames\":[";
                const uint32_t count =
                    std::min<uint32_t>(limit, static_cast<uint32_t>(index.size()));
                for (uint32_t i = 0; i < count; ++i) {
                    if (i != 0) std::cout << ",";
                    std::cout << "{"
                              << "\"frame\":" << index[i].frame_index
                              << ",\"cuCount\":" << index[i].unit_count
                              << ",\"avgQp\":" << static_cast<int>(summaries[i].avg_qp)
                              << ",\"qpMin\":" << static_cast<int>(summaries[i].qp_min)
                              << ",\"qpMax\":" << static_cast<int>(summaries[i].qp_max)
                              << ",\"sliceType\":\"" << slice_type_name(summaries[i].slice_type) << "\""
                              << "}";
                }
                std::cout << "]}";
            }
        }
        std::cout << "}\n";
        return 0;
    }

    std::cout << "VACHUNK " << vachunk_kind_name(h.kind)
              << " " << codec_name(h.codec)
              << " v" << h.version_major << "." << h.version_minor << "\n"
              << "path: " << path << "\n"
              << "track: " << h.track_index
              << ", compression: " << compression_name(h.compression)
              << ", size: " << h.file_size
              << ", base_revision: " << h.base_content_revision
              << ", generator_revision: " << h.generator_revision << "\n"
              << "frames: " << h.start_frame << ".." << h.end_frame
              << ", packets: " << h.start_packet << ".." << h.end_packet
              << ", units: " << h.start_unit << ".." << h.end_unit << "\n"
              << "features:";
    for (const auto& name : features) {
        std::cout << " " << name;
    }
    if (features.empty()) std::cout << " none";
    std::cout << "\nsections:";
    const char* names[] = {"FSUM", "FIDX", "CU4R", "META"};
    for (const char* name : names) {
        const auto* section = chunk.section(name);
        if (section) {
            std::cout << " " << name << "=" << section->entry_count
                      << "x" << section->entry_size;
        }
    }
    std::cout << "\n";

    if (h.kind == static_cast<uint16_t>(VachunkKind::Overlay)) {
        std::vector<VachunkFrameSummary> summaries;
        std::vector<VachunkOverlayFrameIndexEntry> index;
        std::vector<vr::analysis::VachunkCuRecord> records;
        if (read_vachunk_records(chunk, "FSUM", summaries) &&
            read_vachunk_records(chunk, "FIDX", index) &&
            read_vachunk_records(chunk, "CU4R", records)) {
            std::cout << "overlay: frames=" << index.size()
                      << ", cus=" << records.size() << "\n";
            const uint32_t count =
                std::min<uint32_t>(limit, static_cast<uint32_t>(index.size()));
            for (uint32_t i = 0; i < count; ++i) {
                std::cout << "  frame " << index[i].frame_index
                          << " cus=" << index[i].unit_count
                          << " qp=" << static_cast<int>(summaries[i].avg_qp)
                          << " [" << static_cast<int>(summaries[i].qp_min)
                          << "," << static_cast<int>(summaries[i].qp_max) << "]"
                          << " slice=" << slice_type_name(summaries[i].slice_type)
                          << "\n";
            }
        }
    }
    return 0;
}

int inspect_file(const CliOptions& options) {
    const CacheKind kind = detect_kind(options.path);
    if (kind == CacheKind::Vac2) {
        return inspect_vac2(options.path, options.json, options.limit);
    }
    if (kind == CacheKind::Vachunk) {
        return inspect_vachunk(options.path, options.json, options.limit);
    }
    std::cerr << "Unknown cache file magic: " << options.path << "\n";
    return 2;
}

int check_file(const CliOptions& options) {
    std::string error;
    const CacheKind kind = detect_kind(options.path);
    if (kind == CacheKind::Vac2) {
        vr::analysis::Vac2BaseFile vac2;
        const bool ok = vac2.open(options.path) && validate_vac2(vac2, error);
        if (options.json) {
            std::cout << "{\"ok\":" << (ok ? "true" : "false")
                      << ",\"type\":\"vac2\",\"path\":\"" << json_escape(options.path)
                      << "\",\"error\":\"" << json_escape(error) << "\"}\n";
        } else {
            std::cout << (ok ? "OK" : "FAIL") << " VAC2 " << options.path;
            if (!ok) std::cout << ": " << error;
            std::cout << "\n";
        }
        return ok ? 0 : 2;
    }
    if (kind == CacheKind::Vachunk) {
        vr::analysis::VachunkFile chunk;
        const bool ok = chunk.open(options.path) && validate_vachunk(chunk, error);
        if (options.json) {
            std::cout << "{\"ok\":" << (ok ? "true" : "false")
                      << ",\"type\":\"vachunk\",\"path\":\"" << json_escape(options.path)
                      << "\",\"error\":\"" << json_escape(error) << "\"}\n";
        } else {
            std::cout << (ok ? "OK" : "FAIL") << " VACHUNK " << options.path;
            if (!ok) std::cout << ": " << error;
            std::cout << "\n";
        }
        return ok ? 0 : 2;
    }
    std::cerr << "Unknown cache file magic: " << options.path << "\n";
    return 2;
}

int print_vac2_frame(const CliOptions& options) {
    vr::analysis::Vac2BaseFile vac2;
    if (!vac2.open(options.path)) {
        std::cerr << "Failed to open VAC2: " << options.path << "\n";
        return 2;
    }
    if (options.index >= vac2.frames().size()) {
        std::cerr << "Frame index out of range\n";
        return 2;
    }
    const auto& frame = vac2.frames()[options.index];
    const auto& summary = vac2.frame_summaries()[options.index];
    if (options.json) {
        std::cout << "{"
                  << "\"type\":\"vac2Frame\","
                  << "\"path\":\"" << json_escape(options.path) << "\","
                  << "\"index\":" << options.index
                  << ",\"pts\":" << frame.pts
                  << ",\"dts\":" << frame.dts
                  << ",\"duration\":" << frame.duration
                  << ",\"firstPacket\":" << frame.first_packet
                  << ",\"packetCount\":" << frame.packet_count
                  << ",\"firstUnit\":" << frame.first_unit
                  << ",\"unitCount\":" << frame.unit_count
                  << ",\"codedOrder\":" << frame.coded_order
                  << ",\"displayOrder\":" << frame.display_order
                  << ",\"poc\":" << frame.poc
                  << ",\"frameSize\":" << frame.frame_size
                  << ",\"keyframe\":" << ((frame.flags & VAC2_FRAME_FLAG_KEYFRAME) ? "true" : "false")
                  << ",\"sliceType\":\"" << slice_type_name(summary.slice_type) << "\""
                  << ",\"qpKind\":\"" << qp_kind_name(summary.qp_kind) << "\""
                  << ",\"qpAvg\":" << static_cast<int>(summary.qp_avg)
                  << ",\"qpMin\":" << static_cast<int>(summary.qp_min)
                  << ",\"qpMax\":" << static_cast<int>(summary.qp_max)
                  << "}\n";
        return 0;
    }
    std::cout << "VAC2 frame #" << options.index << "\n"
              << "pts=" << frame.pts << ", dts=" << frame.dts
              << ", duration=" << frame.duration << "\n"
              << "packets=" << frame.first_packet << "+" << frame.packet_count
              << ", units=" << frame.first_unit << "+" << frame.unit_count << "\n"
              << "coded_order=" << frame.coded_order
              << ", display_order=" << frame.display_order
              << ", poc=" << frame.poc
              << ", frame_size=" << frame.frame_size << "\n"
              << "slice=" << slice_type_name(summary.slice_type)
              << ", qp=" << static_cast<int>(summary.qp_avg)
              << " [" << static_cast<int>(summary.qp_min)
              << "," << static_cast<int>(summary.qp_max) << "]"
              << " (" << qp_kind_name(summary.qp_kind) << ")"
              << ((frame.flags & VAC2_FRAME_FLAG_KEYFRAME) ? ", keyframe" : "")
              << "\n";
    return 0;
}

int print_chunk_frame(const CliOptions& options) {
    vr::analysis::VachunkFile chunk;
    if (!chunk.open(options.path)) {
        std::cerr << "Failed to open VACHUNK: " << options.path << "\n";
        return 2;
    }
    vr::analysis::VachunkOverlayFrameData frame;
    if (!vr::analysis::read_overlay_vachunk_frame(chunk, options.frame, frame)) {
        std::cerr << "Failed to read overlay frame " << options.frame << "\n";
        return 2;
    }

    uint64_t bit_total = 0;
    uint8_t qp_min = 255;
    uint8_t qp_max = 0;
    for (const auto& cu : frame.cus) {
        bit_total += cu.common.bit_count;
        qp_min = std::min(qp_min, cu.common.qp);
        qp_max = std::max(qp_max, cu.common.qp);
    }
    if (frame.cus.empty()) {
        qp_min = 0;
    }

    if (options.json) {
        std::cout << "{"
                  << "\"type\":\"vachunkOverlayFrame\","
                  << "\"path\":\"" << json_escape(options.path) << "\","
                  << "\"frame\":" << options.frame
                  << ",\"poc\":" << frame.summary.poc
                  << ",\"codedOrder\":" << frame.summary.coded_order
                  << ",\"sliceType\":\"" << slice_type_name(frame.summary.slice_type) << "\""
                  << ",\"avgQp\":" << static_cast<int>(frame.summary.avg_qp)
                  << ",\"qpMin\":" << static_cast<int>(frame.summary.qp_min)
                  << ",\"qpMax\":" << static_cast<int>(frame.summary.qp_max)
                  << ",\"cuCount\":" << frame.cus.size()
                  << ",\"cuQpMin\":" << static_cast<int>(qp_min)
                  << ",\"cuQpMax\":" << static_cast<int>(qp_max)
                  << ",\"bitCountTotal\":" << bit_total
                  << ",\"cus\":[";
        const uint32_t count =
            std::min<uint32_t>(options.limit, static_cast<uint32_t>(frame.cus.size()));
        for (uint32_t i = 0; i < count; ++i) {
            const auto& cu = frame.cus[i];
            if (i != 0) std::cout << ",";
            std::cout << "{"
                      << "\"x\":" << cu.common.x
                      << ",\"y\":" << cu.common.y
                      << ",\"w\":" << static_cast<int>(cu.common.w)
                      << ",\"h\":" << static_cast<int>(cu.common.h)
                      << ",\"depth\":" << static_cast<int>(cu.common.depth)
                      << ",\"qp\":" << static_cast<int>(cu.common.qp)
                      << ",\"predMode\":" << static_cast<int>(cu.common.pred_mode)
                      << ",\"bitCount\":" << cu.common.bit_count
                      << "}";
        }
        std::cout << "]}\n";
        return 0;
    }

    std::cout << "VACHUNK overlay frame " << options.frame << "\n"
              << "poc=" << frame.summary.poc
              << ", coded_order=" << frame.summary.coded_order
              << ", slice=" << slice_type_name(frame.summary.slice_type) << "\n"
              << "summary_qp=" << static_cast<int>(frame.summary.avg_qp)
              << " [" << static_cast<int>(frame.summary.qp_min)
              << "," << static_cast<int>(frame.summary.qp_max) << "]"
              << ", cus=" << frame.cus.size()
              << ", cu_qp=[" << static_cast<int>(qp_min)
              << "," << static_cast<int>(qp_max) << "]"
              << ", bits=" << bit_total << "\n";
    const uint32_t count =
        std::min<uint32_t>(options.limit, static_cast<uint32_t>(frame.cus.size()));
    for (uint32_t i = 0; i < count; ++i) {
        const auto& cu = frame.cus[i];
        std::cout << "  cu#" << i
                  << " x=" << cu.common.x
                  << " y=" << cu.common.y
                  << " " << static_cast<int>(cu.common.w)
                  << "x" << static_cast<int>(cu.common.h)
                  << " d=" << static_cast<int>(cu.common.depth)
                  << " qp=" << static_cast<int>(cu.common.qp)
                  << " pred=" << static_cast<int>(cu.common.pred_mode)
                  << " bits=" << cu.common.bit_count
                  << "\n";
    }
    return 0;
}

int benchmark_overlay(const CliOptions& options) {
    if (options.frame == UINT32_MAX) {
        std::cerr << "Missing --frame N\n";
        return 1;
    }
    if (options.width == 0 || options.height == 0 || options.iterations == 0) {
        std::cerr << "--width, --height, and --iterations must be positive\n";
        return 1;
    }

    vr::analysis::VachunkFile chunk;
    if (!chunk.open(options.path)) {
        std::cerr << "Failed to open VACHUNK: " << options.path << "\n";
        return 2;
    }
    vr::analysis::VachunkOverlayFrameData frame;
    if (!vr::analysis::read_overlay_vachunk_frame(chunk, options.frame, frame)) {
        std::cerr << "Failed to read overlay frame " << options.frame << "\n";
        return 2;
    }

    const auto mode = (options.mode == "qp")
        ? vr::analysis::OverlayHeatmapMode::Qp
        : vr::analysis::OverlayHeatmapMode::BitCost;
    const uint8_t alpha = 255;
    std::vector<uint8_t> pixels(
        static_cast<size_t>(options.width) * static_cast<size_t>(options.height) * 4);
    std::vector<uint8_t> grid_mask;
    if (options.with_grid) {
        grid_mask.resize(static_cast<size_t>(options.width) * static_cast<size_t>(options.height));
    }
    vr::analysis::OverlayRasterStats stats;
    auto raster_grid = [&]() {
        if (!options.with_grid) return;
        const float scale_x = static_cast<float>(options.width) /
            static_cast<float>(options.width);
        const float scale_y = static_cast<float>(options.height) /
            static_cast<float>(options.height);
        for (const auto& cu : frame.cus) {
            const auto& c = cu.common;
            const int x0 = static_cast<int>(std::round(static_cast<float>(c.x) * scale_x));
            const int y0 = static_cast<int>(std::round(static_cast<float>(c.y) * scale_y));
            const int x1 = static_cast<int>(std::round(static_cast<float>(c.x + c.w) * scale_x));
            const int y1 = static_cast<int>(std::round(static_cast<float>(c.y + c.h) * scale_y));
            vr::analysis::stroke_overlay_rect_mask8(
                grid_mask,
                static_cast<int>(options.width),
                static_cast<int>(options.height),
                x0,
                y0,
                x1,
                y1);
        }
    };

    std::fill(pixels.begin(), pixels.end(), 0);
    if (options.with_grid) {
        std::fill(grid_mask.begin(), grid_mask.end(), 0);
    }
    if (!vr::analysis::raster_overlay_heatmap(
            frame,
            options.width,
            options.height,
            static_cast<int>(options.width),
            static_cast<int>(options.height),
            mode,
            alpha,
            pixels,
            &stats)) {
        std::cerr << "Failed to raster overlay frame\n";
        return 2;
    }
    raster_grid();

    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < options.iterations; ++i) {
        std::fill(pixels.begin(), pixels.end(), 0);
        if (options.with_grid) {
            std::fill(grid_mask.begin(), grid_mask.end(), 0);
        }
        if (!vr::analysis::raster_overlay_heatmap(
                frame,
                options.width,
                options.height,
                static_cast<int>(options.width),
                static_cast<int>(options.height),
                mode,
                alpha,
                pixels,
                &stats)) {
            std::cerr << "Failed to raster overlay frame\n";
            return 2;
        }
        raster_grid();
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    const double avg_ms = elapsed / static_cast<double>(options.iterations);
    const uint64_t color_upload_bytes =
        static_cast<uint64_t>(options.width) * static_cast<uint64_t>(options.height) * 4;
    const uint64_t mask_upload_bytes = options.with_grid
        ? static_cast<uint64_t>(options.width) * static_cast<uint64_t>(options.height)
        : 0;

    if (options.json) {
        std::cout << "{"
                  << "\"type\":\"overlayRasterBenchmark\","
                  << "\"path\":\"" << json_escape(options.path) << "\","
                  << "\"frame\":" << options.frame << ","
                  << "\"mode\":\"" << json_escape(options.mode) << "\","
                  << "\"width\":" << options.width << ","
                  << "\"height\":" << options.height << ","
                  << "\"iterations\":" << options.iterations << ","
                  << "\"withGrid\":" << (options.with_grid ? "true" : "false") << ","
                  << "\"cuCount\":" << stats.cu_count << ","
                  << "\"filledPixels\":" << stats.filled_pixels << ","
                  << "\"colorUploadBytes\":" << color_upload_bytes << ","
                  << "\"maskUploadBytes\":" << mask_upload_bytes << ","
                  << "\"totalMs\":" << elapsed << ","
                  << "\"avgMs\":" << avg_ms
                  << "}\n";
    } else {
        std::cout << "Overlay raster benchmark: frame=" << options.frame
                  << " mode=" << options.mode
                  << " iterations=" << options.iterations
                  << " grid=" << (options.with_grid ? "yes" : "no")
                  << " avg=" << avg_ms << " ms"
                  << " cus=" << stats.cu_count
                  << " filled_pixels=" << stats.filled_pixels
                  << " color_upload_bytes=" << color_upload_bytes
                  << " mask_upload_bytes=" << mask_upload_bytes << "\n";
    }
    return 0;
}

int generate_base(const CliOptions& options) {
    if (options.input.empty() || options.cache_root.empty() || options.hash.empty()) {
        std::cerr << "generate-base requires --input, --cache-root, and --hash\n";
        return 1;
    }
    vr::analysis::VacacheStore store(options.cache_root, options.hash);
    if (!store.ensure_layout()) {
        std::cerr << "Failed to create cache layout: " << store.hash_dir() << "\n";
        return 2;
    }
    const std::string tmp_path = join_path(
        store.tmp_dir(), temp_name("base.cli", ".vac.tmp"));
    if (!vr::analysis::AnalysisGenerator::generate_vac2_base(
            options.input, tmp_path, options.max_cache_bytes)) {
        vr::win_utf8::delete_file_utf8(tmp_path);
        std::cerr << "Failed to generate VAC2 base\n";
        return 2;
    }
    if (!replace_file(tmp_path, store.base_path())) {
        vr::win_utf8::delete_file_utf8(tmp_path);
        std::cerr << "Failed to publish VAC2 base: " << store.base_path() << "\n";
        return 2;
    }
    vr::analysis::Vac2BaseFile verify;
    if (!store.open_base(verify)) {
        std::cerr << "Published VAC2 base failed to reopen\n";
        return 2;
    }
    if (options.json) {
        std::cout << "{"
                  << "\"ok\":true,"
                  << "\"type\":\"vac2\","
                  << "\"path\":\"" << json_escape(store.base_path()) << "\","
                  << "\"hash\":\"" << json_escape(options.hash) << "\","
                  << "\"frames\":" << verify.frames().size()
                  << ",\"packets\":" << verify.packets().size()
                  << ",\"units\":" << verify.units().size()
                  << "}\n";
    } else {
        std::cout << "Generated VAC2 base: " << store.base_path() << "\n";
    }
    return 0;
}

int generate_overlay(const CliOptions& options) {
    if (options.input.empty() || options.cache_root.empty() || options.hash.empty() ||
        options.start_frame == UINT32_MAX || options.end_frame == UINT32_MAX ||
        options.start_frame > options.end_frame) {
        std::cerr << "generate-overlay requires --input, --cache-root, --hash, --start-frame, and --end-frame\n";
        return 1;
    }

    vr::analysis::VacacheStore store(options.cache_root, options.hash);
    if (!store.ensure_layout()) {
        std::cerr << "Failed to create cache layout: " << store.hash_dir() << "\n";
        return 2;
    }
    vr::analysis::Vac2BaseFile base;
    if (!store.open_base(base)) {
        std::cerr << "Base VAC2 is missing or invalid: " << store.base_path() << "\n";
        return 2;
    }
    if (options.end_frame >= base.frames().size()) {
        std::cerr << "Frame range exceeds base frame count: " << base.frames().size() << "\n";
        return 2;
    }

    AnalysisCodec codec = static_cast<AnalysisCodec>(base.header().codec);
    if (!options.codec.empty()) {
        codec = parse_codec(options.codec);
    }
    const char* codec_arg = ffmpeg_analysis_codec_arg(codec);
    if (!codec_arg) {
        std::cerr << "generate-overlay only supports h264/hevc/vvc analyzer codecs\n";
        return 2;
    }

    const std::string analyzer = find_analyzer(options.analyzer);
    if (analyzer.empty()) {
        std::cerr << "Could not find void_ffmpeg_analyzer.exe; pass --analyzer or set VOID_FFMPEG_ANALYZER\n";
        return 2;
    }

    const std::string staging_dir = join_path(store.tmp_dir(), temp_name("overlay.cli", ""));
    if (!create_dir(staging_dir)) {
        std::cerr << "Failed to create staging directory: " << staging_dir << "\n";
        return 2;
    }
    const std::string tmp_path = join_path(staging_dir, options.hash + ".overlay.tmp.vck");

    vr::analysis::VachunkKey key;
    key.kind = VachunkKind::Overlay;
    key.codec = codec;
    key.feature_flags = kOverlayVachunkFeatureFlags;
    key.base_content_revision = base.header().content_revision;
    key.generator_revision = options.generator_revision;
    key.start_frame = options.start_frame;
    key.end_frame = options.end_frame;

    const int rc = run_process({
        analyzer,
        "--codec", codec_arg,
        "--input", options.input,
        "--vachunk", tmp_path,
        "--start-frame", std::to_string(options.start_frame),
        "--end-frame", std::to_string(options.end_frame),
        "--base-revision", std::to_string(key.base_content_revision),
        "--generator-revision", std::to_string(key.generator_revision),
    });
    if (rc != 0 || !path_exists(tmp_path)) {
        std::filesystem::remove_all(vr::win_utf8::path_from_utf8(staging_dir));
        std::cerr << "FFmpeg analyzer failed with exit code " << rc << "\n";
        return 2;
    }

    vr::analysis::VachunkFile verify;
    if (!verify.open(tmp_path) || !vachunk_matches_key(verify, key)) {
        std::filesystem::remove_all(vr::win_utf8::path_from_utf8(staging_dir));
        std::cerr << "Generated VACHUNK does not match requested key\n";
        return 2;
    }
    verify.close();

    vr::analysis::VachunkData data;
    if (!vr::analysis::read_vachunk_file_data(tmp_path, data)) {
        std::filesystem::remove_all(vr::win_utf8::path_from_utf8(staging_dir));
        std::cerr << "Failed to read generated VACHUNK for publish\n";
        return 2;
    }
    if (!store.write_chunk_atomic(key, std::move(data), options.max_cache_bytes)) {
        std::filesystem::remove_all(vr::win_utf8::path_from_utf8(staging_dir));
        std::cerr << "Failed to publish VACHUNK\n";
        return 2;
    }
    std::filesystem::remove_all(vr::win_utf8::path_from_utf8(staging_dir));
    if (options.json) {
        std::cout << "{"
                  << "\"ok\":true,"
                  << "\"type\":\"vachunk\","
                  << "\"path\":\"" << json_escape(store.chunk_path(key)) << "\","
                  << "\"hash\":\"" << json_escape(options.hash) << "\","
                  << "\"startFrame\":" << key.start_frame
                  << ",\"endFrame\":" << key.end_frame
                  << ",\"baseRevision\":" << key.base_content_revision
                  << ",\"generatorRevision\":" << key.generator_revision
                  << "}\n";
    } else {
        std::cout << "Generated overlay VACHUNK: " << store.chunk_path(key) << "\n";
    }
    return 0;
}

int run_cli(const std::vector<std::string>& args) {
    CliOptions options;
    if (!parse_args(args, options)) {
        print_usage(std::cerr);
        return 1;
    }
    if (options.command == "help") {
        print_usage(std::cout);
        return 0;
    }
    if (options.command == "generate-base") return generate_base(options);
    if (options.command == "generate-overlay") return generate_overlay(options);
    if (options.path.empty()) {
        std::cerr << "Missing cache file path\n";
        print_usage(std::cerr);
        return 1;
    }
    if (options.command == "inspect") return inspect_file(options);
    if (options.command == "check") return check_file(options);
    if (options.command == "frame") {
        if (options.index == UINT32_MAX) {
            std::cerr << "Missing --index N\n";
            return 1;
        }
        return print_vac2_frame(options);
    }
    if (options.command == "chunk-frame") {
        if (options.frame == UINT32_MAX) {
            std::cerr << "Missing --frame N\n";
            return 1;
        }
        return print_chunk_frame(options);
    }
    if (options.command == "benchmark-overlay") return benchmark_overlay(options);
    std::cerr << "Unknown command: " << options.command << "\n";
    print_usage(std::cerr);
    return 1;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        args.push_back(vr::win_utf8::utf8_from_utf16(argv[i]));
    }
    return run_cli(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return run_cli(args);
}
#endif

#include "analysis/cache/overlay_chunk.h"
#include "analysis/parsers/vac2_parser.h"
#include "analysis/parsers/vachunk_parser.h"
#include "common/win_utf8.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <process.h>
#include <string>
#include <vector>

namespace {

struct TempFixture {
    std::filesystem::path dir;
    std::string vac_path;
    std::string vck_path;
};

bool fail(const std::string& message) {
    std::cerr << message << "\n";
    return false;
}

vr::analysis::Vac2BaseData make_vac2_data() {
    vr::analysis::Vac2BaseData data;
    data.codec = AnalysisCodec::HEVC;
    data.track_index = 0;
    data.time_base_num = 1;
    data.time_base_den = 1000;
    data.width = 64;
    data.height = 64;
    data.source_size = 1024;
    data.source_mtime_unix_ms = 123456789;
    data.content_revision = 7;
    data.metadata_json = R"({"schema":"cli-smoke"})";

    Vac2PacketEntry packet0{};
    packet0.pts = 0;
    packet0.dts = 0;
    packet0.duration = 40;
    packet0.size = 100;
    packet0.flags = VAC2_PACKET_FLAG_KEYFRAME;
    packet0.first_unit = 0;
    packet0.unit_count = 1;
    packet0.au_index = 0;

    Vac2PacketEntry packet1{};
    packet1.pts = 40;
    packet1.dts = 40;
    packet1.duration = 40;
    packet1.size = 80;
    packet1.first_unit = 1;
    packet1.unit_count = 1;
    packet1.au_index = 1;
    data.packets = {packet0, packet1};

    Vac2BitstreamUnitEntry unit0{};
    unit0.packet_index = 0;
    unit0.au_index = 0;
    unit0.offset = 0;
    unit0.size = 100;
    unit0.nal_type = 19;
    unit0.unit_kind = static_cast<uint8_t>(AnalysisUnitKind::Nalu);
    unit0.flags = VAC2_UNIT_FLAG_IS_VCL |
                  VAC2_UNIT_FLAG_IS_SLICE |
                  VAC2_UNIT_FLAG_IS_KEYFRAME;

    Vac2BitstreamUnitEntry unit1{};
    unit1.packet_index = 1;
    unit1.au_index = 1;
    unit1.offset = 100;
    unit1.size = 80;
    unit1.nal_type = 1;
    unit1.unit_kind = static_cast<uint8_t>(AnalysisUnitKind::Nalu);
    unit1.flags = VAC2_UNIT_FLAG_IS_VCL | VAC2_UNIT_FLAG_IS_SLICE;
    data.units = {unit0, unit1};

    Vac2FrameEntry frame0{};
    frame0.first_packet = 0;
    frame0.packet_count = 1;
    frame0.first_unit = 0;
    frame0.unit_count = 1;
    frame0.pts = 0;
    frame0.dts = 0;
    frame0.duration = 40;
    frame0.coded_order = 0;
    frame0.display_order = 0;
    frame0.poc = 0;
    frame0.frame_size = 100;
    frame0.flags = VAC2_FRAME_FLAG_KEYFRAME | VAC2_FRAME_FLAG_RAP;

    Vac2FrameEntry frame1{};
    frame1.first_packet = 1;
    frame1.packet_count = 1;
    frame1.first_unit = 1;
    frame1.unit_count = 1;
    frame1.pts = 40;
    frame1.dts = 40;
    frame1.duration = 40;
    frame1.coded_order = 1;
    frame1.display_order = 1;
    frame1.poc = 2;
    frame1.frame_size = 80;
    frame1.rap_distance = 1;
    data.frames = {frame0, frame1};

    Vac2FrameSummaryEntry summary0{};
    summary0.poc = 0;
    summary0.coded_order = 0;
    summary0.first_vcl_unit = 0;
    summary0.flags = VAC2_FRAME_SUMMARY_FLAG_EXACT_QP;
    summary0.slice_type = 2;
    summary0.nal_type = 19;
    summary0.qp_kind = VAC2_QP_KIND_EXACT;
    summary0.qp_avg = 22;
    summary0.qp_min = 20;
    summary0.qp_max = 24;

    Vac2FrameSummaryEntry summary1{};
    summary1.poc = 2;
    summary1.coded_order = 1;
    summary1.first_vcl_unit = 1;
    summary1.slice_type = 1;
    summary1.nal_type = 1;
    summary1.qp_kind = VAC2_QP_KIND_EXACT;
    summary1.qp_avg = 30;
    summary1.qp_min = 29;
    summary1.qp_max = 31;
    data.frame_summaries = {summary0, summary1};

    return data;
}

vr::analysis::VachunkData make_vachunk_data() {
    std::vector<VachunkFrameSummary> summaries;
    std::vector<VachunkOverlayFrameIndexEntry> index;
    std::vector<vr::analysis::VachunkCuRecord> records;

    for (uint32_t i = 0; i < 2; ++i) {
        VachunkFrameSummary summary{};
        summary.poc = static_cast<int32_t>(i * 2);
        summary.coded_order = i;
        summary.vcl_unit_index = i;
        summary.slice_type = i == 0 ? 2 : 1;
        summary.avg_qp = static_cast<uint8_t>(22 + i);
        summary.qp_min = static_cast<uint8_t>(20 + i);
        summary.qp_max = static_cast<uint8_t>(24 + i);
        summary.num_cus = 1;
        summary.cu_index_entry = i;
        summaries.push_back(summary);

        VachunkOverlayFrameIndexEntry frame_index{};
        frame_index.frame_index = i;
        frame_index.first_unit = i;
        frame_index.unit_count = 1;
        frame_index.flags =
            VACHUNK_OVERLAY_FRAME_FLAG_COMPLETE |
            VACHUNK_OVERLAY_FRAME_FLAG_EXACT;
        index.push_back(frame_index);

        vr::analysis::VachunkCuRecord cu{};
        cu.common.x = static_cast<uint16_t>(i * 16);
        cu.common.y = 0;
        cu.common.w = 16;
        cu.common.h = 16;
        cu.common.qp = summary.avg_qp;
        cu.common.pred_mode = i == 0 ? 1 : 0;
        cu.common.bit_count = 128 + i;
        records.push_back(cu);
    }

    vr::analysis::VachunkData data;
    data.kind = VachunkKind::Overlay;
    data.codec = AnalysisCodec::HEVC;
    data.feature_flags = VACHUNK_FEATURE_CU_GEOMETRY |
                         VACHUNK_FEATURE_QP |
                         VACHUNK_FEATURE_PRED_MODE |
                         VACHUNK_FEATURE_BIT_COST;
    data.base_content_revision = 7;
    data.generator_revision = 3;
    data.start_frame = 0;
    data.end_frame = 1;
    data.sections.push_back(
        vr::analysis::make_vachunk_record_section("FSUM", summaries));
    data.sections.push_back(
        vr::analysis::make_vachunk_record_section("FIDX", index));
    data.sections.push_back(
        vr::analysis::make_vachunk_record_section("CU4R", records));
    return data;
}

bool write_fixtures(TempFixture& fixture) {
    fixture.dir = std::filesystem::temp_directory_path() /
                  ("voidplayer_cli_smoke_" + std::to_string(std::rand()));
    std::error_code ec;
    std::filesystem::create_directories(fixture.dir, ec);
    if (ec) return fail("failed to create temp fixture directory");

    fixture.vac_path = vr::win_utf8::path_to_utf8(fixture.dir / L"base.vac");
    fixture.vck_path = vr::win_utf8::path_to_utf8(fixture.dir / L"overlay.vck");
    if (!vr::analysis::write_vac2_base_container(fixture.vac_path, make_vac2_data())) {
        return fail("failed to write VAC2 fixture");
    }
    if (!vr::analysis::write_vachunk_file(fixture.vck_path, make_vachunk_data())) {
        return fail("failed to write VACHUNK fixture");
    }
    return true;
}

bool run_cli(const std::string& cli_path,
             const std::vector<std::string>& args) {
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(cli_path);
    storage.insert(storage.end(), args.begin(), args.end());

    std::vector<const char*> argv;
    argv.reserve(storage.size() + 1);
    for (const auto& arg : storage) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    const intptr_t exit_code = _spawnv(_P_WAIT, cli_path.c_str(), argv.data());
    if (exit_code != 0) {
        std::cerr << "CLI command failed (" << exit_code << "):";
        for (const auto& arg : args) {
            std::cerr << " " << arg;
        }
        std::cerr << "\n";
        return false;
    }
    return true;
}

bool run_cli_expect_exit(const std::string& cli_path,
                         const std::vector<std::string>& args,
                         intptr_t expected_exit_code) {
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(cli_path);
    storage.insert(storage.end(), args.begin(), args.end());

    std::vector<const char*> argv;
    argv.reserve(storage.size() + 1);
    for (const auto& arg : storage) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    const intptr_t exit_code = _spawnv(_P_WAIT, cli_path.c_str(), argv.data());
    if (exit_code != expected_exit_code) {
        std::cerr << "CLI command exited " << exit_code
                  << ", expected " << expected_exit_code << ":";
        for (const auto& arg : args) {
            std::cerr << " " << arg;
        }
        std::cerr << "\n";
        return false;
    }
    return true;
}

std::string quote_command_arg(const std::string& arg) {
    std::string quoted = "\"";
    for (const char ch : arg) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted += ch;
        }
    }
    quoted += "\"";
    return quoted;
}

bool run_cli_expect_output(
    const std::string& cli_path,
    const std::vector<std::string>& args,
    int expected_exit_code,
    const std::vector<std::string>& expected_fragments) {
    std::string command = quote_command_arg(cli_path);
    for (const auto& arg : args) {
        command += " " + quote_command_arg(arg);
    }
    command = "\"" + command + "\"";

    FILE* pipe = _popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return fail("failed to capture CLI output");
    }
    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    const int exit_code = _pclose(pipe);
    if (exit_code != expected_exit_code) {
        std::cerr << "CLI command exited " << exit_code
                  << ", expected " << expected_exit_code << ":"
                  << command << "\n";
        return false;
    }
    for (const auto& fragment : expected_fragments) {
        if (output.find(fragment) == std::string::npos) {
            std::cerr << "CLI output missing " << fragment << ": "
                      << output << "\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return fail("usage: analysis_cli_smoke_tests <VoidPlayerCli.exe>") ? 0 : 1;
    }
    const std::string cli_path = argv[1];
    const std::string analyzer_path = argc >= 3 ? argv[2] : std::string{};
    const std::string video_path = argc >= 4 ? argv[3] : std::string{};
    TempFixture fixture;
    if (!write_fixtures(fixture)) return 1;

    const bool ok =
        run_cli(cli_path, {"inspect", fixture.vac_path, "--json"}) &&
        run_cli(cli_path, {"check", fixture.vac_path, "--json"}) &&
        run_cli(cli_path, {"frame", fixture.vac_path, "--index", "1", "--json"}) &&
        run_cli(cli_path, {"inspect", fixture.vck_path, "--json"}) &&
        run_cli(cli_path, {"check", fixture.vck_path, "--json"}) &&
        run_cli(cli_path, {"chunk-frame", fixture.vck_path, "--frame", "1", "--json"}) &&
        run_cli(cli_path, {
            "benchmark-overlay",
            fixture.vck_path,
            "--frame", "1",
            "--width", "64",
            "--height", "64",
            "--iterations", "2",
            "--with-grid",
            "--json",
        }) &&
        run_cli_expect_exit(cli_path, {
            "benchmark-overlay-gpu",
            fixture.vck_path,
            "--frame", "1",
            "--width", "64",
            "--height", "64",
            "--iterations", "2",
            "--with-grid",
            "--json",
        }, 2) &&
        run_cli_expect_output(cli_path, {
            "score-quality",
            "--bad-flag",
            "--json",
        }, 1, {
            "\"type\":\"qualityError\"",
            "\"code\":\"invalid_arguments\"",
            "\"message\":\"failed to parse score-quality arguments\"",
        });

    bool generation_ok = true;
    if (!analyzer_path.empty() && !video_path.empty()) {
        const std::string generated_root =
            vr::win_utf8::path_to_utf8(fixture.dir / L"generated-cache");
        generation_ok =
            run_cli(cli_path, {
                "generate-base",
                "--input", video_path,
                "--cache-root", generated_root,
                "--hash", "cli_generated",
                "--json",
            }) &&
            run_cli(cli_path, {
                "generate-overlay",
                "--input", video_path,
                "--cache-root", generated_root,
                "--hash", "cli_generated",
                "--start-frame", "0",
                "--end-frame", "0",
                "--codec", "hevc",
                "--analyzer", analyzer_path,
                "--json",
            }) &&
            run_cli(cli_path, {
                "inspect",
                vr::win_utf8::path_to_utf8(
                    fixture.dir / L"generated-cache" /
                    L"cli_generated" / L"base.vac"),
                "--json",
            }) &&
            run_cli(cli_path, {
                "score-quality",
                "--input", video_path,
                "--backend", "cpu",
                "--decode-threads", "2",
                "--cpu-workers", "2",
                "--cpu-in-flight", "2",
                "--sample-interval-ms", "250",
                "--max-samples", "2",
                "--json",
            }) &&
            run_cli_expect_output(cli_path, {
                "score-quality",
                "--input", video_path,
                "--backend", "cpu",
                "--max-samples", "1",
                "--json",
                "--summary-only",
            }, 0, {
                "\"maxSamples\":1",
                "\"truncated\":true",
                "\"sampledFrames\":1",
            }) &&
            run_cli(cli_path, {
                "score-quality",
                "--input", video_path,
                "--backend", "cpu",
                "--max-samples", "2",
                "--jsonl",
            }) &&
            run_cli_expect_exit(cli_path, {
                "score-quality",
                "--json",
            }, 1);
    }

    std::error_code ec;
    std::filesystem::remove_all(fixture.dir, ec);
    return ok && generation_ok ? 0 : 1;
}

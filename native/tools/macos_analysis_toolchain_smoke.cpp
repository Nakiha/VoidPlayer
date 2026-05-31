#include "analysis/cache/vacache_store.h"
#include "analysis/cache/overlay_chunk.h"
#include "analysis/parsers/vac2_parser.h"
#include "analysis/parsers/vachunk_parser.h"
#include "common/win_utf8.h"
#include "tools/test_video_assets.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr uint64_t kOverlayVachunkFeatureFlags =
    VACHUNK_FEATURE_CU_GEOMETRY |
    VACHUNK_FEATURE_QP |
    VACHUNK_FEATURE_PRED_MODE |
    VACHUNK_FEATURE_MOTION_VECTORS |
    VACHUNK_FEATURE_REF_INDEXES |
    VACHUNK_FEATURE_BIT_COST;

bool fail(const std::string& message) {
    std::cerr << message << "\n";
    return false;
}

int run_process(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(args[0].c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

std::filesystem::path make_temp_cache_root() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ticks =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    std::ostringstream name;
    name << "voidplayer-macos-analysis-toolchain-" << static_cast<long long>(getpid())
         << "-" << ticks;
    return std::filesystem::temp_directory_path() / name.str();
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

bool run_cli(const std::string& cli, const std::vector<std::string>& args) {
    std::vector<std::string> command;
    command.reserve(args.size() + 1);
    command.push_back(cli);
    command.insert(command.end(), args.begin(), args.end());
    const int rc = run_process(command);
    if (rc != 0) {
        std::cerr << "command failed with exit code " << rc << ":";
        for (const auto& arg : command) std::cerr << " " << arg;
        std::cerr << "\n";
        return false;
    }
    return true;
}

bool run_smoke(const std::string& cli,
               const std::string& analyzer,
               const std::string& video_root) {
    if (!path_exists(cli)) return fail("VoidPlayerCli is missing: " + cli);
    if (!path_exists(analyzer)) return fail("void_ffmpeg_analyzer is missing: " + analyzer);
    const std::string video = vp_tools::h264_smoke_video_path(video_root);
    if (!path_exists(video)) return fail("sample video is missing: " + video);

    const std::filesystem::path cache_root = make_temp_cache_root();
    std::filesystem::create_directories(cache_root);
    const std::string hash = "macos-analysis-toolchain-smoke";
    vr::analysis::VacacheStore store(
        vr::win_utf8::path_to_utf8(cache_root),
        hash);

    const auto cleanup = [&]() {
        std::error_code ec;
        std::filesystem::remove_all(cache_root, ec);
    };

    if (!run_cli(cli, {
            "generate-base",
            "--input", video,
            "--cache-root", vr::win_utf8::path_to_utf8(cache_root),
            "--hash", hash,
            "--json",
        })) {
        cleanup();
        return false;
    }

    vr::analysis::Vac2BaseFile base;
    if (!store.open_base(base)) {
        cleanup();
        return fail("generated VAC2 base failed to reopen");
    }
    if (base.frames().empty() || base.packets().empty() || base.units().empty()) {
        cleanup();
        return fail("generated VAC2 base has empty frame/packet/unit tables");
    }

    const uint32_t end_frame =
        std::min<uint32_t>(3, static_cast<uint32_t>(base.frames().size() - 1));
    if (!run_cli(cli, {
            "generate-overlay",
            "--input", video,
            "--cache-root", vr::win_utf8::path_to_utf8(cache_root),
            "--hash", hash,
            "--start-frame", "0",
            "--end-frame", std::to_string(end_frame),
            "--codec", "h264",
            "--analyzer", analyzer,
            "--json",
        })) {
        cleanup();
        return false;
    }

    vr::analysis::VachunkKey key;
    key.kind = VachunkKind::Overlay;
    key.codec = AnalysisCodec::H264;
    key.feature_flags = kOverlayVachunkFeatureFlags;
    key.base_content_revision = base.header().content_revision;
    key.generator_revision = 3;
    key.start_frame = 0;
    key.end_frame = end_frame;

    const std::string chunk_path = store.chunk_path(key);
    vr::analysis::VachunkFile chunk;
    if (!chunk.open(chunk_path)) {
        cleanup();
        return fail("generated overlay VACHUNK failed to reopen");
    }
    if (chunk.header().start_frame != 0 || chunk.header().end_frame != end_frame) {
        cleanup();
        return fail("generated VACHUNK frame range does not match request");
    }
    vr::analysis::VachunkOverlayFrameData frame;
    if (!vr::analysis::read_overlay_vachunk_frame(chunk, 0, frame)) {
        cleanup();
        return fail("generated VACHUNK frame 0 is unreadable");
    }
    if (frame.cus.empty()) {
        cleanup();
        return fail("generated VACHUNK frame 0 has no CU/MB records");
    }

    if (!run_cli(cli, {"check", store.base_path(), "--json"}) ||
        !run_cli(cli, {"check", chunk_path, "--json"})) {
        cleanup();
        return false;
    }

    cleanup();
    std::cout << "macos_analysis_toolchain_smoke passed\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: macos_analysis_toolchain_smoke "
                     "<VoidPlayerCli> <void_ffmpeg_analyzer> <video-test-dir>\n";
        return 1;
    }
    return run_smoke(argv[1], argv[2], argv[3]) ? 0 : 2;
}

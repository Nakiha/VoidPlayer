// Pipeline benchmark main entry point.
// Individual stages are in bench_*.cpp.

#include "bench_common.h"
#include <iostream>
#include <string>
#include <cmath>

int main(int argc, char* argv[]) {
    std::string video_path;
    if (argc > 1) {
        video_path = argv[1];
    } else {
        video_path = "resources/video/h264_9s_1920x1080.mp4";
    }

    std::cout << "Video Pipeline Benchmark\n";
    std::cout << "File: " << video_path << "\n\n";

    auto r1  = bench_demux_only(video_path);
    print_result(r1);

    auto r2  = bench_demux_decode(video_path);
    print_result(r2);

    auto r3  = bench_demux_decode_sws(video_path);
    print_result(r3);

    auto r4  = bench_demux_decode_sws_d3d11(video_path);
    print_result(r4);

    auto r4b = bench_demux_decode_sws_d3d11_reuse(video_path);
    print_result(r4b);

    auto r5  = bench_full_pipeline(video_path);
    print_result(r5);

    // Summary comparison
    std::cout << "\n=== Summary ===\n";
    std::cout << "  Demux only:                   " << r1.packets_per_sec << " pkt/s  (" << r1.elapsed_ms << " ms)\n";
    std::cout << "  Demux + Decode:               " << r2.fps << " fps    (" << r2.elapsed_ms << " ms)\n";
    std::cout << "  Demux + Decode + sws_scale:   " << r3.fps << " fps    (" << r3.elapsed_ms << " ms)\n";
    std::cout << "  + D3D11 upload (new texture):  " << r4.fps << " fps    (" << r4.elapsed_ms << " ms)\n";
    std::cout << "  + D3D11 upload (reuse):        " << r4b.fps << " fps    (" << r4b.elapsed_ms << " ms)\n";
    std::cout << "  Full pipeline (Present):       " << r5.elapsed_ms << " ms  (realtime x" << r5.fps << ")\n";

    // Identify bottleneck (based on offline stages 1-4)
    double decode_ms = r2.elapsed_ms - r1.elapsed_ms;
    double sws_ms = r3.elapsed_ms - r2.elapsed_ms;
    double upload_ms = r4.elapsed_ms - r3.elapsed_ms;
    double total = r4.elapsed_ms;

    std::cout << "\n=== Time per stage ===\n";
    std::cout << "  Demux:    " << r1.elapsed_ms << " ms (" << (r1.elapsed_ms / total * 100) << "%)\n";
    std::cout << "  Decode:   " << decode_ms << " ms (" << (decode_ms / total * 100) << "%)\n";
    std::cout << "  sws_scale: " << sws_ms << " ms (" << (sws_ms / total * 100) << "%)\n";
    std::cout << "  D3D11:    " << upload_ms << " ms (" << (upload_ms / total * 100) << "%)\n";

    if (sws_ms > decode_ms && sws_ms > upload_ms) {
        std::cout << "\n  BOTTLENECK: sws_scale (YUV->RGBA conversion)\n";
    } else if (decode_ms > upload_ms) {
        std::cout << "\n  BOTTLENECK: Decode (avcodec)\n";
    } else {
        std::cout << "\n  BOTTLENECK: D3D11 upload (texture creation/upload)\n";
    }

    double reuse_saving = r4.elapsed_ms - r4b.elapsed_ms;
    std::cout << "  Texture reuse saves: " << reuse_saving << " ms ("
              << (reuse_saving / r4.elapsed_ms * 100) << "%)\n";

    return 0;
}

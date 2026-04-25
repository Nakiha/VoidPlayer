#pragma once
#include <iostream>
#include <chrono>
#include <string>

struct BenchResult {
    std::string name;
    int total_frames = 0;
    int total_packets = 0;
    double elapsed_ms = 0;
    double fps = 0;
    double packets_per_sec = 0;
    double bytes_processed = 0; // MB
};

inline void print_result(const BenchResult& r) {
    std::cout << "\n=== " << r.name << " ===\n";
    if (r.total_packets > 0)
        std::cout << "  Packets:    " << r.total_packets << "\n";
    if (r.total_frames > 0)
        std::cout << "  Frames:     " << r.total_frames << "\n";
    std::cout << "  Time:       " << r.elapsed_ms << " ms\n";
    if (r.total_frames > 0)
        std::cout << "  FPS:        " << r.fps << "\n";
    if (r.total_packets > 0)
        std::cout << "  Pkts/sec:   " << r.packets_per_sec << "\n";
    if (r.bytes_processed > 0)
        std::cout << "  Data:       " << r.bytes_processed << " MB\n";
    if (r.total_frames > 0 && r.elapsed_ms > 0)
        std::cout << "  Ms/frame:   " << (r.elapsed_ms / r.total_frames) << "\n";
}

BenchResult bench_demux_only(const std::string& path);
BenchResult bench_demux_decode(const std::string& path);
BenchResult bench_demux_decode_sws(const std::string& path);
BenchResult bench_demux_decode_sws_d3d11(const std::string& path);
BenchResult bench_demux_decode_sws_d3d11_reuse(const std::string& path);
BenchResult bench_full_pipeline(const std::string& path);

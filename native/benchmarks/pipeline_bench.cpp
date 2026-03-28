// Pipeline stage benchmarks for video renderer
// Tests each stage independently to identify bottlenecks:
//   Stage 1: Demux only (read all packets)
//   Stage 2: Demux + Decode (decode all frames)
//   Stage 3: Demux + Decode + sws_scale (convert to RGBA)
//   Stage 4: Demux + Decode + sws_scale + D3D11 upload

#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <d3d11.h>
#include <dxgi.h>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif

// ==================== Utilities ====================

struct BenchResult {
    std::string name;
    int total_frames = 0;
    int total_packets = 0;
    double elapsed_ms = 0;
    double fps = 0;
    double packets_per_sec = 0;
    double bytes_processed = 0; // MB
};

void print_result(const BenchResult& r) {
    std::cout << "\n=== " << r.name << " ===\n";
    std::cout << "  Packets:    " << r.total_packets << "\n";
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

// ==================== Stage 1: Demux Only ====================

BenchResult bench_demux_only(const std::string& path) {
    BenchResult result;
    result.name = "Stage 1: Demux Only";

    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr);
    if (ret < 0) { std::cerr << "Failed to open: " << path << "\n"; return result; }
    avformat_find_stream_info(fmt_ctx, nullptr);

    int video_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = static_cast<int>(i);
            break;
        }
    }

    AVPacket* pkt = av_packet_alloc();
    auto t0 = std::chrono::high_resolution_clock::now();

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            result.total_packets++;
            result.bytes_processed += pkt->size;
        }
        av_packet_unref(pkt);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    result.packets_per_sec = result.total_packets / (result.elapsed_ms / 1000.0);
    result.bytes_processed /= (1024.0 * 1024.0);

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    return result;
}

// ==================== Stage 2: Demux + Decode ====================

BenchResult bench_demux_decode(const std::string& path) {
    BenchResult result;
    result.name = "Stage 2: Demux + Decode";

    AVFormatContext* fmt_ctx = nullptr;
    avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr);
    avformat_find_stream_info(fmt_ctx, nullptr);

    int video_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = static_cast<int>(i);
            break;
        }
    }

    AVStream* stream = fmt_ctx->streams[video_idx];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    avcodec_open2(codec_ctx, codec, nullptr);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    auto t0 = std::chrono::high_resolution_clock::now();

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }
        result.total_packets++;

        int ret = avcodec_send_packet(codec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            result.total_frames++;
            result.bytes_processed += frame->width * frame->height * 1.5; // approximate YUV420P
            av_frame_unref(frame);
        }
    }

    // Flush decoder
    avcodec_send_packet(codec_ctx, nullptr);
    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
        result.total_frames++;
        result.bytes_processed += frame->width * frame->height * 1.5;
        av_frame_unref(frame);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    result.fps = result.total_frames / (result.elapsed_ms / 1000.0);
    result.packets_per_sec = result.total_packets / (result.elapsed_ms / 1000.0);
    result.bytes_processed /= (1024.0 * 1024.0);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return result;
}

// ==================== Stage 3: Demux + Decode + sws_scale ====================

BenchResult bench_demux_decode_sws(const std::string& path) {
    BenchResult result;
    result.name = "Stage 3: Demux + Decode + sws_scale (YUV→RGBA)";

    AVFormatContext* fmt_ctx = nullptr;
    avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr);
    avformat_find_stream_info(fmt_ctx, nullptr);

    int video_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = static_cast<int>(i);
            break;
        }
    }

    AVStream* stream = fmt_ctx->streams[video_idx];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    avcodec_open2(codec_ctx, codec, nullptr);

    int w = codec_ctx->width;
    int h = codec_ctx->height;

    SwsContext* sws = sws_getContext(w, h, codec_ctx->pix_fmt,
                                      w, h, AV_PIX_FMT_RGBA,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);

    // Pre-allocate output buffer
    size_t stride = static_cast<size_t>(w) * 4;
    std::vector<uint8_t> rgba_buf(stride * h);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    auto t0 = std::chrono::high_resolution_clock::now();

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }
        result.total_packets++;

        int ret = avcodec_send_packet(codec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            uint8_t* dst_slices[1] = { rgba_buf.data() };
            int dst_stride[1] = { static_cast<int>(stride) };

            int converted = sws_scale(sws,
                frame->data, frame->linesize,
                0, h, dst_slices, dst_stride);

            result.total_frames++;
            result.bytes_processed += w * h * 4; // RGBA
            av_frame_unref(frame);
        }
    }

    // Flush
    avcodec_send_packet(codec_ctx, nullptr);
    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
        uint8_t* dst_slices[1] = { rgba_buf.data() };
        int dst_stride[1] = { static_cast<int>(stride) };
        sws_scale(sws, frame->data, frame->linesize, 0, h, dst_slices, dst_stride);
        result.total_frames++;
        result.bytes_processed += w * h * 4;
        av_frame_unref(frame);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    result.fps = result.total_frames / (result.elapsed_ms / 1000.0);
    result.packets_per_sec = result.total_packets / (result.elapsed_ms / 1000.0);
    result.bytes_processed /= (1024.0 * 1024.0);

    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return result;
}

// ==================== Stage 4: Demux + Decode + sws_scale + D3D11 Upload ====================

BenchResult bench_demux_decode_sws_d3d11(const std::string& path) {
    BenchResult result;
    result.name = "Stage 4: Demux + Decode + sws_scale + D3D11 Upload";

    // Create D3D11 device (no window needed)
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL feature_level;
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, nullptr, 0, D3D11_SDK_VERSION,
                                   &device, &feature_level, &context);
    if (FAILED(hr)) {
        // Retry without VIDEO_SUPPORT
        flags = 0;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               flags, nullptr, 0, D3D11_SDK_VERSION,
                               &device, &feature_level, &context);
    }
    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 device\n";
        return result;
    }

    AVFormatContext* fmt_ctx = nullptr;
    avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr);
    avformat_find_stream_info(fmt_ctx, nullptr);

    int video_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = static_cast<int>(i);
            break;
        }
    }

    AVStream* stream = fmt_ctx->streams[video_idx];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    avcodec_open2(codec_ctx, codec, nullptr);

    int w = codec_ctx->width;
    int h = codec_ctx->height;

    SwsContext* sws = sws_getContext(w, h, codec_ctx->pix_fmt,
                                      w, h, AV_PIX_FMT_RGBA,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);

    size_t stride = static_cast<size_t>(w) * 4;
    std::vector<uint8_t> rgba_buf(stride * h);

    // Pre-create texture description
    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = static_cast<UINT>(w);
    tex_desc.Height = static_cast<UINT>(h);
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DYNAMIC;
    tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    auto t0 = std::chrono::high_resolution_clock::now();

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }
        result.total_packets++;

        int ret = avcodec_send_packet(codec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            // sws_scale
            uint8_t* dst_slices[1] = { rgba_buf.data() };
            int dst_stride[1] = { static_cast<int>(stride) };
            sws_scale(sws, frame->data, frame->linesize, 0, h, dst_slices, dst_stride);

            // Create D3D11 texture and upload
            ID3D11Texture2D* tex = nullptr;
            device->CreateTexture2D(&tex_desc, nullptr, &tex);

            D3D11_MAPPED_SUBRESOURCE mapped = {};
            context->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            UINT row_bytes = static_cast<UINT>(w) * 4;
            const uint8_t* src_row = rgba_buf.data();
            uint8_t* dst_row = static_cast<uint8_t*>(mapped.pData);
            for (int y = 0; y < h; ++y) {
                memcpy(dst_row, src_row, row_bytes);
                src_row += stride;
                dst_row += mapped.RowPitch;
            }
            context->Unmap(tex, 0);

            result.total_frames++;
            result.bytes_processed += w * h * 4;

            tex->Release();
            av_frame_unref(frame);
        }
    }

    // Flush
    avcodec_send_packet(codec_ctx, nullptr);
    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
        uint8_t* dst_slices[1] = { rgba_buf.data() };
        int dst_stride[1] = { static_cast<int>(stride) };
        sws_scale(sws, frame->data, frame->linesize, 0, h, dst_slices, dst_stride);

        ID3D11Texture2D* tex = nullptr;
        device->CreateTexture2D(&tex_desc, nullptr, &tex);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        context->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        UINT row_bytes = static_cast<UINT>(w) * 4;
        const uint8_t* src_row = rgba_buf.data();
        uint8_t* dst_row = static_cast<uint8_t*>(mapped.pData);
        for (int y = 0; y < h; ++y) {
            memcpy(dst_row, src_row, row_bytes);
            src_row += stride;
            dst_row += mapped.RowPitch;
        }
        context->Unmap(tex, 0);
        result.total_frames++;
        result.bytes_processed += w * h * 4;
        tex->Release();
        av_frame_unref(frame);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    result.fps = result.total_frames / (result.elapsed_ms / 1000.0);
    result.packets_per_sec = result.total_packets / (result.elapsed_ms / 1000.0);
    result.bytes_processed /= (1024.0 * 1024.0);

    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    context->Release();
    device->Release();
    return result;
}

// ==================== Stage 4b: Same as 4 but reuse texture ====================

BenchResult bench_demux_decode_sws_d3d11_reuse(const std::string& path) {
    BenchResult result;
    result.name = "Stage 4b: Demux + Decode + sws_scale + D3D11 Upload (texture reuse)";

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL feature_level;
    UINT flags = 0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, nullptr, 0, D3D11_SDK_VERSION,
                                   &device, &feature_level, &context);
    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 device\n";
        return result;
    }

    AVFormatContext* fmt_ctx = nullptr;
    avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr);
    avformat_find_stream_info(fmt_ctx, nullptr);

    int video_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = static_cast<int>(i);
            break;
        }
    }

    AVStream* stream = fmt_ctx->streams[video_idx];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    avcodec_open2(codec_ctx, codec, nullptr);

    int w = codec_ctx->width;
    int h = codec_ctx->height;

    SwsContext* sws = sws_getContext(w, h, codec_ctx->pix_fmt,
                                      w, h, AV_PIX_FMT_RGBA,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);

    size_t stride = static_cast<size_t>(w) * 4;
    std::vector<uint8_t> rgba_buf(stride * h);

    // Create ONE texture and reuse it
    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = static_cast<UINT>(w);
    tex_desc.Height = static_cast<UINT>(h);
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DYNAMIC;
    tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* tex = nullptr;
    device->CreateTexture2D(&tex_desc, nullptr, &tex);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    auto t0 = std::chrono::high_resolution_clock::now();

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }
        result.total_packets++;

        int ret = avcodec_send_packet(codec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            uint8_t* dst_slices[1] = { rgba_buf.data() };
            int dst_stride[1] = { static_cast<int>(stride) };
            sws_scale(sws, frame->data, frame->linesize, 0, h, dst_slices, dst_stride);

            // Reuse same texture
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            context->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            UINT row_bytes = static_cast<UINT>(w) * 4;
            const uint8_t* src_row = rgba_buf.data();
            uint8_t* dst_row = static_cast<uint8_t*>(mapped.pData);
            for (int y = 0; y < h; ++y) {
                memcpy(dst_row, src_row, row_bytes);
                src_row += stride;
                dst_row += mapped.RowPitch;
            }
            context->Unmap(tex, 0);

            result.total_frames++;
            result.bytes_processed += w * h * 4;
            av_frame_unref(frame);
        }
    }

    // Flush
    avcodec_send_packet(codec_ctx, nullptr);
    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
        uint8_t* dst_slices[1] = { rgba_buf.data() };
        int dst_stride[1] = { static_cast<int>(stride) };
        sws_scale(sws, frame->data, frame->linesize, 0, h, dst_slices, dst_stride);

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        context->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        UINT row_bytes = static_cast<UINT>(w) * 4;
        const uint8_t* src_row = rgba_buf.data();
        uint8_t* dst_row = static_cast<uint8_t*>(mapped.pData);
        for (int y = 0; y < h; ++y) {
            memcpy(dst_row, src_row, row_bytes);
            src_row += stride;
            dst_row += mapped.RowPitch;
        }
        context->Unmap(tex, 0);
        result.total_frames++;
        result.bytes_processed += w * h * 4;
        av_frame_unref(frame);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    result.fps = result.total_frames / (result.elapsed_ms / 1000.0);
    result.packets_per_sec = result.total_packets / (result.elapsed_ms / 1000.0);
    result.bytes_processed /= (1024.0 * 1024.0);

    tex->Release();
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    context->Release();
    device->Release();
    return result;
}

// ==================== Main ====================

int main(int argc, char* argv[]) {
    // Determine video path
    std::string video_path;
    if (argc > 1) {
        video_path = argv[1];
    } else {
        // Default: look for test video
        video_path = "resources/video/h264_9s_1920x1080.mp4";
    }

    std::cout << "Video Pipeline Benchmark\n";
    std::cout << "File: " << video_path << "\n\n";

    auto r1 = bench_demux_only(video_path);
    print_result(r1);

    auto r2 = bench_demux_decode(video_path);
    print_result(r2);

    auto r3 = bench_demux_decode_sws(video_path);
    print_result(r3);

    auto r4 = bench_demux_decode_sws_d3d11(video_path);
    print_result(r4);

    auto r4b = bench_demux_decode_sws_d3d11_reuse(video_path);
    print_result(r4b);

    // Summary comparison
    std::cout << "\n=== Summary ===\n";
    std::cout << "  Demux only:                   " << r1.fps << " pkt/s  (" << r1.elapsed_ms << " ms)\n";
    std::cout << "  Demux + Decode:               " << r2.fps << " fps    (" << r2.elapsed_ms << " ms)\n";
    std::cout << "  Demux + Decode + sws_scale:   " << r3.fps << " fps    (" << r3.elapsed_ms << " ms)\n";
    std::cout << "  + D3D11 upload (new texture):  " << r4.fps << " fps    (" << r4.elapsed_ms << " ms)\n";
    std::cout << "  + D3D11 upload (reuse):        " << r4b.fps << " fps    (" << r4b.elapsed_ms << " ms)\n";

    // Identify bottleneck
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
        std::cout << "\n  BOTTLENECK: sws_scale (YUV→RGBA conversion)\n";
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

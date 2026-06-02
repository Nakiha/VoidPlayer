#include "bench_common.h"
#include <chrono>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <d3d11.h>
#include <dxgi.h>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif

// Helper: run the demux+decode+sws+upload loop with a given texture strategy.
// create_each_frame=true: new texture per frame. false: reuse one texture.
static BenchResult run_upload_bench(const std::string& path, bool create_each_frame) {
    BenchResult result;
    result.name = create_each_frame
        ? "Stage 4: Demux + Decode + sws_scale + D3D11 Upload"
        : "Stage 4b: Demux + Decode + sws_scale + D3D11 Upload (texture reuse)";

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL feature_level;
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, nullptr, 0, D3D11_SDK_VERSION,
                                   &device, &feature_level, &context);
    if (FAILED(hr)) {
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

    ID3D11Texture2D* reuse_tex = nullptr;
    if (!create_each_frame) {
        device->CreateTexture2D(&tex_desc, nullptr, &reuse_tex);
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    auto t0 = std::chrono::high_resolution_clock::now();

    auto upload_frame = [&](ID3D11Texture2D* tex) {
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
    };

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

            if (create_each_frame) {
                ID3D11Texture2D* tex = nullptr;
                device->CreateTexture2D(&tex_desc, nullptr, &tex);
                upload_frame(tex);
                tex->Release();
            } else {
                upload_frame(reuse_tex);
            }

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

        if (create_each_frame) {
            ID3D11Texture2D* tex = nullptr;
            device->CreateTexture2D(&tex_desc, nullptr, &tex);
            upload_frame(tex);
            tex->Release();
        } else {
            upload_frame(reuse_tex);
        }

        result.total_frames++;
        result.bytes_processed += w * h * 4;
        av_frame_unref(frame);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    result.fps = result.total_frames / (result.elapsed_ms / 1000.0);
    result.packets_per_sec = result.total_packets / (result.elapsed_ms / 1000.0);
    result.bytes_processed /= (1024.0 * 1024.0);

    if (reuse_tex) reuse_tex->Release();
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    context->Release();
    device->Release();
    return result;
}

BenchResult bench_demux_decode_sws_d3d11(const std::string& path) {
    return run_upload_bench(path, true);
}

BenchResult bench_demux_decode_sws_d3d11_reuse(const std::string& path) {
    return run_upload_bench(path, false);
}

#include "video_renderer/renderer.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>

namespace vr {

Renderer::Renderer() = default;

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::initialize(const RendererConfig& config) {
    // Configure logging if non-default settings provided
    configure_logging(config.log_config);

    // Install crash handler if file path is set
    if (!config.log_config.file_path.empty()) {
        // Extract directory from file path for crash logs
        std::string crash_dir;
        auto last_sep = config.log_config.file_path.find_last_of("/\\");
        if (last_sep != std::string::npos) {
            crash_dir = config.log_config.file_path.substr(0, last_sep);
        }
        install_crash_handler(crash_dir);
    }

    if (config.video_paths.empty()) {
        spdlog::error("Renderer: no video paths provided");
        return false;
    }

    hwnd_ = config.hwnd;
    target_width_ = config.width;
    target_height_ = config.height;

    // Initialize D3D11 device
    d3d_device_ = std::make_unique<D3D11Device>();
    if (!d3d_device_->initialize(hwnd_, target_width_, target_height_)) {
        spdlog::error("Renderer: failed to initialize D3D11 device");
        return false;
    }

    texture_mgr_ = std::make_unique<TextureManager>(d3d_device_->device(), d3d_device_->context());
    shader_mgr_ = std::make_unique<ShaderManager>(d3d_device_->device());

    // Compile shaders
    std::string shader_dir = VR_SHADER_DIR;
    std::string shader_path = shader_dir + "/multitrack.hlsl";
    if (!shader_mgr_->compile_from_file(shader_path, "VSMain", "PSMain", compiled_shader_)) {
        spdlog::error("Renderer: failed to compile shaders");
        return false;
    }

    // Create constant buffer for shader uniforms (must be 16-byte aligned)
    // Layout must match multitrack.hlsl cbuffer Constants
    if (!shader_mgr_->create_constant_buffer(d3d_device_->device(), 64, compiled_shader_)) {
        spdlog::error("Renderer: failed to create constant buffer");
        return false;
    }

    // Create sampler state
    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    d3d_device_->device()->CreateSamplerState(&sampler_desc, &sampler_state_);

    // Create vertex buffer (fullscreen quad)
    struct Vertex { float x, y, u, v; };
    Vertex quad[] = {
        {-1, -1, 0, 1},  // bottom-left (UV flipped for D3D)
        {-1,  1, 0, 0},  // top-left
        { 1, -1, 1, 1},  // bottom-right
        { 1,  1, 1, 0},  // top-right
    };
    D3D11_BUFFER_DESC vb_desc = {};
    vb_desc.ByteWidth = sizeof(quad);
    vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA vb_data = {};
    vb_data.pSysMem = quad;
    d3d_device_->device()->CreateBuffer(&vb_desc, &vb_data, &vertex_buffer_);

    // Create tracks
    for (const auto& path : config.video_paths) {
        auto pipeline = std::make_unique<TrackPipeline>();
        pipeline->file_path = path;
        pipeline->packet_queue = std::make_unique<PacketQueue>(100);
        pipeline->track_buffer = std::make_unique<TrackBuffer>(30, 2);
        pipeline->demux_thread = std::make_unique<DemuxThread>(path, *pipeline->packet_queue);

        if (!pipeline->demux_thread->start()) {
            spdlog::error("Renderer: failed to start demux for {}", path);
            continue;
        }

        // Wait briefly for stats to be available
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const auto& stats = pipeline->demux_thread->stats();
        if (stats.video_stream_index < 0) {
            spdlog::error("Renderer: no video stream found in {}", path);
            continue;
        }

        pipeline->decode_thread = std::make_unique<DecodeThread>(
            *pipeline->packet_queue,
            *pipeline->track_buffer,
            stats.codec_params,
            stats.time_base
        );

        // Try hardware decode if requested
        if (config.use_hardware_decode) {
            pipeline->decode_thread->enable_hardware_decode(d3d_device_->device(),
                                                            &device_mutex_);
        }

        if (!pipeline->decode_thread->start()) {
            spdlog::error("Renderer: failed to start decode for {}", path);
            pipeline->demux_thread->stop();  // Stop already-started demux thread
            continue;
        }

        tracks_.push_back(std::move(pipeline));
    }

    if (tracks_.empty()) {
        spdlog::error("Renderer: no valid tracks");
        return false;
    }

    // Setup render sink
    render_sink_ = std::make_unique<RenderSink>(clock_);
    for (auto& track : tracks_) {
        render_sink_->add_track(track->track_buffer.get());
    }

    initialized_ = true;
    spdlog::info("Renderer: initialized with {} tracks", tracks_.size());
    return true;
}

void Renderer::shutdown() {
    running_ = false;
    playing_ = false;

    if (render_thread_.joinable()) {
        render_thread_.join();
    }

    // Stop all tracks
    for (auto& track : tracks_) {
        if (track->decode_thread) track->decode_thread->stop();
        if (track->demux_thread) track->demux_thread->stop();
    }
    tracks_.clear();

    render_sink_.reset();
    shader_mgr_.reset();
    texture_mgr_.reset();

    if (vertex_buffer_) { vertex_buffer_->Release(); vertex_buffer_ = nullptr; }
    if (sampler_state_) { sampler_state_->Release(); sampler_state_ = nullptr; }
    if (cached_rtv_) { cached_rtv_->Release(); cached_rtv_ = nullptr; }

    if (d3d_device_) {
        d3d_device_->shutdown();
        d3d_device_.reset();
    }

    initialized_ = false;
    spdlog::info("Renderer: shutdown complete");
}

void Renderer::play() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_) return;

    clock_.play();
    playing_ = true;

    if (!running_) {
        running_ = true;
        render_thread_ = std::thread(&Renderer::render_loop, this);
    }
}

void Renderer::pause() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    clock_.pause();
    playing_ = false;
}

void Renderer::resume() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    clock_.resume();
    playing_ = true;
}

void Renderer::seek(int64_t target_pts_us) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    clock_.seek(target_pts_us);

    // Seek all tracks — delegate seek to DemuxThread so it runs on the demux
    // thread itself (AVFormatContext is not thread-safe)
    for (auto& track : tracks_) {
        track->track_buffer->set_state(TrackState::Flushing);
        track->track_buffer->clear_frames();      // Discard stale decoded frames
        track->packet_queue->flush();
        track->demux_thread->seek(target_pts_us);
        track->track_buffer->set_state(TrackState::Buffering);
    }
}

void Renderer::set_speed(double speed) {
    clock_.set_speed(speed);
}

bool Renderer::is_playing() const {
    return playing_;
}

bool Renderer::is_initialized() const {
    return initialized_;
}

int64_t Renderer::current_pts_us() const {
    return clock_.current_pts_us();
}

double Renderer::current_speed() const {
    return clock_.speed();
}

size_t Renderer::track_count() const {
    return tracks_.size();
}

int64_t Renderer::duration_us() const {
    int64_t max_dur = 0;
    for (const auto& track : tracks_) {
        max_dur = std::max(max_dur, track->demux_thread->stats().duration_us);
    }
    return max_dur;
}

void Renderer::render_loop() {
    spdlog::info("[Renderer] Render loop started");

    while (running_) {
        // Preroll: keep clock paused while any track is still buffering
        bool any_buffering = false;
        for (auto& track : tracks_) {
            if (track->track_buffer->state() == TrackState::Buffering) {
                any_buffering = true;
                break;
            }
        }
        if (any_buffering && !clock_.is_paused()) {
            clock_.pause();
            spdlog::debug("[Renderer] Preroll: clock paused, waiting for buffers to fill");
        } else if (!any_buffering && clock_.is_paused() && playing_) {
            clock_.resume();
            preview_drawn_ = false;  // Reset for next seek/restart
            spdlog::info("[Renderer] Preroll complete, all tracks ready");
        }

        if (!playing_ || clock_.is_paused()) {
            // While paused/prerolling, draw the first available frame as a static preview
            if (playing_ && any_buffering) {
                bool has_any_frame = false;
                PresentDecision preview;
                preview.current_pts_us = 0;
                preview.should_present = false;
                preview.frames.resize(tracks_.size());
                for (size_t t = 0; t < tracks_.size(); ++t) {
                    auto frame = tracks_[t]->track_buffer->peek(0);
                    if (frame.has_value()) {
                        preview.frames[t] = frame;
                        has_any_frame = true;
                    }
                }
                if (has_any_frame && !preview_drawn_) {
                    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
                    draw_frame(preview);
                    d3d_device_->present(0);
                    preview_drawn_ = true;
                    spdlog::debug("[Renderer] Preroll: drew first frame preview");
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto decision = render_sink_->evaluate();

        if (decision.should_present) {
            std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
            draw_frame(decision);
            d3d_device_->present(0);
        }

        // Sleep until next frame check
        int64_t frame_duration = 16666; // ~60fps
        double spd = clock_.speed();
        if (spd > 0) {
            frame_duration = static_cast<int64_t>(frame_duration / spd);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(frame_duration));
    }

    spdlog::info("[Renderer] Render loop ended");
}

void Renderer::draw_frame(const PresentDecision& decision) {
    auto* ctx = d3d_device_->context();

    // Get or create cached render target view
    if (!cached_rtv_) {
        ID3D11Texture2D* back_buffer = nullptr;
        HRESULT hr = d3d_device_->swap_chain()->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                              reinterpret_cast<void**>(&back_buffer));
        if (FAILED(hr)) {
            spdlog::error("[Renderer] Failed to get back buffer: HRESULT {:#x}", static_cast<unsigned long>(hr));
            return;
        }
        hr = d3d_device_->device()->CreateRenderTargetView(back_buffer, nullptr, &cached_rtv_);
        back_buffer->Release();
        if (FAILED(hr)) {
            spdlog::error("[Renderer] Failed to create RTV: HRESULT {:#x}", static_cast<unsigned long>(hr));
            return;
        }
    }

    float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(cached_rtv_, clear_color);
    ctx->OMSetRenderTargets(1, &cached_rtv_, nullptr);

    // Setup viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(target_width_);
    vp.Height = static_cast<float>(target_height_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Setup input assembler
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vertex_buffer_, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    if (compiled_shader_.layout) {
        ctx->IASetInputLayout(compiled_shader_.layout.Get());
    }

    // Set shaders
    ctx->VSSetShader(compiled_shader_.vs.Get(), nullptr, 0);
    ctx->PSSetShader(compiled_shader_.ps.Get(), nullptr, 0);

    // Update constant buffer
    // Layout must match HLSL cbuffer Constants in multitrack.hlsl
    if (compiled_shader_.constant_buffer) {
        struct Constants {
            int track_count;        // offset 0
            float canvas_width;     // offset 4
            float canvas_height;    // offset 8
            float _pad0;            // offset 12 (padding to 16-byte boundary)
            float video_aspect[4];  // offset 16 (16-byte boundary)
            int nv12_mask;          // offset 32: bit i = track i is NV12
            float _pad1[3];         // offset 36-47: padding
            float nv12_uv_scale_y[4]; // offset 48-63: video_h / texture_h
        };
        static_assert(sizeof(Constants) == 64, "Constants must be 64 bytes");
        Constants cb = {};
        cb.track_count = static_cast<int>(decision.frames.size());
        cb.canvas_width = static_cast<float>(target_width_);
        cb.canvas_height = static_cast<float>(target_height_);
        cb.nv12_mask = 0;
        for (size_t i = 0; i < decision.frames.size() && i < 4; ++i) {
            const auto& s = tracks_[i]->demux_thread->stats();
            if (s.width > 0 && s.height > 0) {
                cb.video_aspect[i] = static_cast<float>(s.width) / static_cast<float>(s.height);
            } else {
                cb.video_aspect[i] = 16.0f / 9.0f;
            }
            if (decision.frames[i].has_value() && decision.frames[i]->is_nv12) {
                cb.nv12_mask |= (1 << static_cast<int>(i));
            }
            cb.nv12_uv_scale_y[i] = tracks_[i]->nv12_uv_scale_y;
        }
        ctx->UpdateSubresource(compiled_shader_.constant_buffer.Get(), 0, nullptr, &cb, 0, 0);
        ctx->PSSetConstantBuffers(0, 1, compiled_shader_.constant_buffer.GetAddressOf());
    }

    // Set sampler
    if (sampler_state_) {
        ctx->PSSetSamplers(0, 1, &sampler_state_);
    }

    // Set textures from frames
    ID3D11ShaderResourceView* srvs[4] = {};           // t0-t3: RGBA (sw) or full NV12 (hw)
    ID3D11ShaderResourceView* nv12_y_srvs[4] = {};    // t4-t7: NV12 Y plane
    ID3D11ShaderResourceView* nv12_uv_srvs[4] = {};   // t8-t11: NV12 UV plane

    for (size_t i = 0; i < decision.frames.size() && i < 4; ++i) {
        if (!decision.frames[i].has_value() || !decision.frames[i]->texture_handle) continue;
        const auto& frame = decision.frames[i].value();
        auto& track = tracks_[i];

        if (frame.is_ref && frame.is_nv12) {
            // D3D11VA NV12 hardware decode: texture_handle is ID3D11Texture2D*
            // Cache SRVs — only recreate when the hw texture pointer or array index changes.
            auto* nv12_tex = static_cast<ID3D11Texture2D*>(frame.texture_handle);
            int array_idx = static_cast<int>(frame.texture_array_index);

            if (track->last_nv12_tex != nv12_tex || track->last_nv12_idx != array_idx) {
                // Texture or slice changed — recreate SRVs
                // Also compute UV scale factor to crop alignment padding
                D3D11_TEXTURE2D_DESC tex_desc = {};
                nv12_tex->GetDesc(&tex_desc);
                if (tex_desc.Height > 0 && frame.height > 0 && tex_desc.Height != static_cast<UINT>(frame.height)) {
                    track->nv12_uv_scale_y = static_cast<float>(frame.height) / static_cast<float>(tex_desc.Height);
                } else {
                    track->nv12_uv_scale_y = 1.0f;
                }
                track->nv12_y_srv.Reset();
                track->nv12_uv_srv.Reset();

                D3D11_SHADER_RESOURCE_VIEW_DESC y_desc = {};
                y_desc.Format = DXGI_FORMAT_R8_UNORM;
                y_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                y_desc.Texture2DArray.MipLevels = 1;
                y_desc.Texture2DArray.FirstArraySlice = static_cast<UINT>(array_idx);
                y_desc.Texture2DArray.ArraySize = 1;
                HRESULT hr = d3d_device_->device()->CreateShaderResourceView(
                    nv12_tex, &y_desc, &track->nv12_y_srv);
                if (FAILED(hr)) {
                    spdlog::error("[Renderer] Failed to create NV12 Y SRV for track {}: {:#x}",
                                  i, static_cast<unsigned long>(hr));
                }

                D3D11_SHADER_RESOURCE_VIEW_DESC uv_desc = {};
                uv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
                uv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                uv_desc.Texture2DArray.MipLevels = 1;
                uv_desc.Texture2DArray.FirstArraySlice = static_cast<UINT>(array_idx);
                uv_desc.Texture2DArray.ArraySize = 1;
                hr = d3d_device_->device()->CreateShaderResourceView(
                    nv12_tex, &uv_desc, &track->nv12_uv_srv);
                if (FAILED(hr)) {
                    spdlog::error("[Renderer] Failed to create NV12 UV SRV for track {}: {:#x}",
                                  i, static_cast<unsigned long>(hr));
                }

                track->last_nv12_tex = nv12_tex;
                track->last_nv12_idx = array_idx;
            }

            nv12_y_srvs[i] = track->nv12_y_srv.Get();
            nv12_uv_srvs[i] = track->nv12_uv_srv.Get();

        } else if (frame.is_ref) {
            // Non-NV12 hardware texture (future use): create single SRV
            auto* tex = static_cast<ID3D11Texture2D*>(frame.texture_handle);
            srvs[i] = texture_mgr_->create_srv(tex);

        } else {
            // Software decode: reuse a pooled RGBA texture
            int w = frame.width > 0 ? frame.width : target_width_;
            int h = frame.height > 0 ? frame.height : target_height_;

            // Recreate pool texture if dimensions changed
            bool need_new_tex = !track->sw_texture;
            if (track->sw_texture) {
                D3D11_TEXTURE2D_DESC existing_desc = {};
                track->sw_texture->GetDesc(&existing_desc);
                if (static_cast<int>(existing_desc.Width) != w || static_cast<int>(existing_desc.Height) != h) {
                    need_new_tex = true;
                }
            }
            if (need_new_tex) {
                track->sw_srv.Reset();
                track->sw_texture.Attach(texture_mgr_->create_rgba_texture(w, h));
                if (track->sw_texture) {
                    track->sw_srv.Attach(texture_mgr_->create_srv(track->sw_texture.Get()));
                }
            }

            if (track->sw_texture && track->sw_srv) {
                int stride = w * 4;
                texture_mgr_->upload_data(track->sw_texture.Get(),
                    static_cast<const uint8_t*>(frame.texture_handle),
                    w, h, stride);
                srvs[i] = track->sw_srv.Get();
            }
        }
    }

    // Bind SRVs: t0-t3 RGBA, t4-t7 NV12 Y, t8-t11 NV12 UV
    ctx->PSSetShaderResources(0, 4, srvs);
    ctx->PSSetShaderResources(4, 4, nv12_y_srvs);
    ctx->PSSetShaderResources(8, 4, nv12_uv_srvs);

    // Draw
    ctx->Draw(4, 0);

    // Unbind SRVs before releasing to avoid GPU resource-in-use issues
    ID3D11ShaderResourceView* null_srvs[4] = {};
    ctx->PSSetShaderResources(0, 4, null_srvs);
    ctx->PSSetShaderResources(4, 4, null_srvs);
    ctx->PSSetShaderResources(8, 4, null_srvs);

    // Cleanup temporary SRVs (only non-ref hw textures created via create_srv)
    for (size_t i = 0; i < 4; ++i) {
        // NV12 Y/UV SRVs are cached in TrackPipeline — do not release here.
        // sw SRVs are also cached — do not release.
        // Only release SRVs that were created for non-NV12 ref textures.
        if (srvs[i]) {
            bool is_cached = false;
            if (i < tracks_.size()) {
                is_cached = (srvs[i] == tracks_[i]->sw_srv.Get());
            }
            if (!is_cached) {
                srvs[i]->Release();
            }
        }
    }
    // NOTE: cached_rtv_ is reused across frames, released in shutdown()
}

} // namespace vr

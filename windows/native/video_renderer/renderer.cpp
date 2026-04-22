#include "video_renderer/renderer.h"
#include "embedded_shaders.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace vr {

// Max sleep for responsiveness (allows seek/pause within ~50 ms)
static constexpr int64_t MAX_SLEEP_US = 8000;  // 8ms cap → ~120Hz layout response

Renderer::Renderer() = default;

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::initialize(const RendererConfig& config) {
    // Flutter plugin configures logging before initialize().
    // Skip empty config to avoid clearing all sinks.
    if (!config.log_config.file_path.empty() || config.log_config.level != spdlog::level::info) {
        configure_logging(config.log_config);
    }

    // Install crash handler if file path is set
    if (!config.log_config.file_path.empty()) {
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
    headless_ = config.headless;
    target_width_ = config.width;
    target_height_ = config.height;

    d3d_device_ = std::make_unique<D3D11Device>();
    if (config.headless) {
        if (!d3d_device_->initialize_headless(config.dxgi_adapter, target_width_, target_height_)) {
            spdlog::error("Renderer: failed to initialize D3D11 device (headless)");
            return false;
        }
        if (!create_shared_buffers(target_width_, target_height_,
                                    dbuf_.textures, dbuf_.rtvs, dbuf_.handles)) {
            return false;
        }
        dbuf_.front.store(0);

        D3D11_QUERY_DESC fence_desc = {};
        fence_desc.Query = D3D11_QUERY_EVENT;
        HRESULT hr = d3d_device_->device()->CreateQuery(&fence_desc, &gpu_fence_);
        if (FAILED(hr)) {
            spdlog::error("Renderer: failed to create GPU fence: HRESULT {:#x}", static_cast<unsigned long>(hr));
            return false;
        }

        spdlog::info("Renderer: headless mode, triple-buffered {}x{} BGRA, handles=[{}, {}, {}]",
                     target_width_, target_height_,
                     reinterpret_cast<uintptr_t>(dbuf_.handles[0]),
                     reinterpret_cast<uintptr_t>(dbuf_.handles[1]),
                     reinterpret_cast<uintptr_t>(dbuf_.handles[2]));
    } else {
        if (!d3d_device_->initialize(hwnd_, target_width_, target_height_)) {
            spdlog::error("Renderer: failed to initialize D3D11 device");
            return false;
        }
    }

    texture_mgr_ = std::make_unique<TextureManager>(d3d_device_->device(), d3d_device_->context());
    shader_mgr_ = std::make_unique<ShaderManager>(d3d_device_->device());

    if (!shader_mgr_->compile_from_source(kMultitrackHlsl, "VSMain", "PSMain", compiled_shader_)) {
        spdlog::error("Renderer: failed to compile shaders");
        return false;
    }

    // Create constant buffer for shader uniforms (must be 16-byte aligned)
    // Layout must match multitrack.hlsl cbuffer Constants
    if (!shader_mgr_->create_constant_buffer(d3d_device_->device(), 208, compiled_shader_)) {
        spdlog::error("Renderer: failed to create constant buffer");
        return false;
    }

    // Create sampler state
    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
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
        int slot = find_empty_slot();
        if (slot < 0) {
            spdlog::warn("Renderer: skipping {}, max {} tracks", path, kMaxTracks);
            continue;
        }

        auto pipeline = create_pipeline(path, config.use_hardware_decode);
        if (!pipeline) continue;

        pipeline->file_id = next_file_id_++;
        tracks_[slot] = std::move(pipeline);
    }

    bool any_track = false;
    for (const auto& t : tracks_) { if (t) { any_track = true; break; } }
    if (!any_track) {
        spdlog::error("Renderer: no valid tracks");
        return false;
    }

    // Initialize file_id_order_ and slot-based order
    {
        int pos = 0;
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (tracks_[i]) {
                file_id_order_[pos] = tracks_[i]->file_id;
                layout_.order[pos] = static_cast<int>(i);
                ++pos;
            }
        }
        for (int i = pos; i < 4; ++i) {
            file_id_order_[i] = -1;
            layout_.order[i] = 0;
        }
    }

    // Setup render sink
    render_sink_ = std::make_unique<RenderSink>(clock_);
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) {
            render_sink_->set_track(i, tracks_[i]->track_buffer.get());
        }
    }

    // Cache duration (immutable after init)
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) {
            cached_duration_us_ = std::max(cached_duration_us_,
                tracks_[i]->demux_thread->stats().duration_us);
        }
    }

    initialized_ = true;

    // Initialize perf stats baseline
    stats_start_time_ = std::chrono::steady_clock::now();
    for (auto& bl : perf_baselines_) bl.frames = 0;

    // Start render loop immediately (paused mode).
    // Decodes and displays first frame, fills buffers, but does not advance playback.
    running_ = true;
    render_thread_ = std::thread(&Renderer::render_loop, this);

    spdlog::info("Renderer: initialized with {} tracks", track_count());
    return true;
}

void Renderer::shutdown() {
    running_ = false;
    playing_ = false;

    if (render_thread_.joinable()) {
        render_thread_.join();
    }

    // Clear cached frames that may hold hw decode surface references.
    // Must happen before decode_thread->stop() frees hw_device_ctx,
    // otherwise hw_frame_ref cleanup will access a freed device context.
    last_decision_ = PresentDecision();

    // Stop all tracks
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) {
            tracks_[i]->decode_thread->stop();
            tracks_[i]->demux_thread->stop();
            tracks_[i].reset();
        }
    }

    render_sink_.reset();
    shader_mgr_.reset();
    texture_mgr_.reset();

    // ComPtr auto-releases — just reset
    cached_rtv_.Reset();
    sampler_state_.Reset();
    vertex_buffer_.Reset();

    if (d3d_device_) {
        d3d_device_->shutdown();
        d3d_device_.reset();
    }

    initialized_ = false;
    spdlog::info("Renderer: shutdown complete");
}

void Renderer::play() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_ || playing_) return;

    clock_.resume();
    playing_ = true;
}

void Renderer::pause() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    clock_.pause();
    playing_ = false;
}

void Renderer::seek(int64_t target_pts_us, SeekType type) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    seek_internal(target_pts_us, type);
}

void Renderer::seek_internal(int64_t target_pts_us, SeekType type) {
    // Caller must hold state_mutex_
    spdlog::info("[Renderer] seek_internal: target={:.3f}s, type={}",
                 target_pts_us / 1e6, type == SeekType::Exact ? "Exact" : "Keyframe");
    clock_.seek(target_pts_us);

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) continue;
        auto& track = tracks_[i];
        // Per-track target: subtract this track's sync offset
        int64_t track_target = std::max(target_pts_us - track->offset_us, int64_t(0));
        // Pause decoder FIRST to prevent stale packets from reaching the codec
        // (avoids HEVC "Could not find ref" warnings during seek transition)
        track->decode_thread->set_decode_paused(true);
        auto buf_count_before = track->track_buffer->total_count();
        auto pq_size_before = track->packet_queue->size();
        track->track_buffer->set_state(TrackState::Flushing);
        track->track_buffer->clear_frames();
        track->packet_queue->flush();
        track->seek_controller->request_seek(track_target, type);
        track->track_buffer->set_state(TrackState::Buffering);
        spdlog::info("[Renderer] seek_internal: track[{}] cleared (buf={}->{}, pq={}->0), state->Buffering, target={:.3f}s",
                     i, buf_count_before, track->track_buffer->total_count(), pq_size_before, track_target / 1e6);
    }
    preview_drawn_ = false;
    last_decision_ = PresentDecision();
}

void Renderer::step_forward() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!initialized_) return;

        clock_.pause();
        playing_ = false;

        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!tracks_[i]) continue;
            auto& track = tracks_[i];
            auto current = track->track_buffer->peek(0);
            if (!current.has_value()) continue;
            track->track_buffer->advance();
            auto next = track->track_buffer->peek(0);
            if (next.has_value()) {
                clock_.seek(next->pts_us);
            }
        }
    }
    draw_paused_frame("step_forward");
}

void Renderer::step_backward() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!initialized_) return;

        // If any track is still seeking/seeking, don't allow stepping
        // (prevents retreating to stale frames during async seek)
        // Exception: tracks past their duration (Ready + no frames) don't block.
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!tracks_[i]) continue;
            auto& buf = tracks_[i]->track_buffer;
            if (buf->state() == TrackState::Buffering) return;
        }

        clock_.pause();
        playing_ = false;

        // Check if ALL tracks can retreat (cache hit)
        bool all_can_retreat = true;
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!tracks_[i]) continue;
            if (!tracks_[i]->track_buffer->can_retreat()) {
                all_can_retreat = false;
                break;
            }
        }

        if (all_can_retreat) {
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!tracks_[i]) continue;
                tracks_[i]->track_buffer->retreat();
            }
            int ref = first_active_track();
            if (ref >= 0) {
                auto frame = tracks_[ref]->track_buffer->peek(0);
                if (frame.has_value()) {
                    clock_.seek(frame->pts_us);
                }
            }
        } else {
            // Cache miss: exact seek to (current_pts - frame_duration - margin)
            // Add 1ms margin: frame duration is integer-truncated (e.g. 1/60s → 16666us)
            // but actual PTS spacing is 16667us, so (pts - dur) overshoots the
            // previous frame by 1us and exact seek's "< target" check discards it.
            int64_t dur = compute_frame_duration_us();
            int64_t target = std::max(int64_t(0),
                clock_.current_pts_us() - dur - 1000);
            spdlog::info("[Renderer] step_backward exact_seek: pts={:.3f}s, duration={:.3f}ms, target={:.3f}s",
                         clock_.current_pts_us() / 1e6, dur / 1e3, target / 1e6);
            seek_internal(target, SeekType::Exact);
            spdlog::info("[Renderer] step_backward exact_seek done: clock_pts={:.3f}s",
                         clock_.current_pts_us() / 1e6);
            // Don't draw stale frame — seek_internal set preview_drawn_=false,
            // render loop will draw the new frame when decode completes.
            return;
        }
    }
    draw_paused_frame("step_backward");
}

void Renderer::draw_paused_frame(const char* reason) {
    PresentDecision decision;
    decision.current_pts_us = 0;
    decision.should_present = false;
    bool has_frame = false;
    for (size_t t = 0; t < kMaxTracks; ++t) {
        if (!tracks_[t]) {
            decision.frames[t] = std::nullopt;
            continue;
        }
        auto frame = tracks_[t]->track_buffer->peek(0);
        if (frame.has_value()) {
            decision.frames[t] = frame;
            has_frame = true;
        }
    }
    if (!has_frame && has_any_frame(last_decision_)) {
        decision = last_decision_;
        has_frame = true;
    }
    if (has_frame) {
        present_frame(decision);
        last_decision_ = decision;
        int ref = first_active_track();
        double pts = (ref >= 0 && decision.frames[ref].has_value())
                     ? decision.frames[ref]->pts_us / 1e6 : -1.0;
        spdlog::info("[Renderer] draw_paused_frame({}): pts={:.3f}s", reason, pts);
    }
}

int Renderer::pick_free_buffer() const {
    int front = dbuf_.front.load();
    return (front + 2) % SharedBuffers::kCount;
}

void Renderer::wait_gpu_and_swap(int back, const char* label) {
    auto* ctx = d3d_device_->context();
    ctx->End(gpu_fence_.Get());
    auto fence_start = std::chrono::steady_clock::now();
    int spin_count = 0;
    while (ctx->GetData(gpu_fence_.Get(), nullptr, 0, 0) == S_FALSE) {
        SwitchToThread();
        if (++spin_count >= 256) {
            spin_count = 0;
            if (std::chrono::steady_clock::now() - fence_start > std::chrono::milliseconds(100)) {
                spdlog::warn("[{}] GPU fence timeout after 100ms", label);
                break;
            }
        }
    }
    dbuf_.front.store(back);
}

bool Renderer::create_shared_buffers(
    int width, int height,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[],
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvs[],
    HANDLE handles[])
{
    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = static_cast<UINT>(width);
    tex_desc.Height = static_cast<UINT>(height);
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    for (int i = 0; i < SharedBuffers::kCount; ++i) {
        HRESULT hr = d3d_device_->device()->CreateTexture2D(&tex_desc, nullptr, &textures[i]);
        if (FAILED(hr)) {
            spdlog::error("Renderer: failed to create shared texture[{}]: HRESULT {:#x}", i, static_cast<unsigned long>(hr));
            return false;
        }
        hr = d3d_device_->device()->CreateRenderTargetView(textures[i].Get(), nullptr, &rtvs[i]);
        if (FAILED(hr)) {
            spdlog::error("Renderer: failed to create shared RTV[{}]: HRESULT {:#x}", i, static_cast<unsigned long>(hr));
            return false;
        }
        Microsoft::WRL::ComPtr<IDXGIResource> dxgi_resource;
        hr = textures[i].As(&dxgi_resource);
        if (SUCCEEDED(hr)) {
            hr = dxgi_resource->GetSharedHandle(&handles[i]);
            if (FAILED(hr)) {
                spdlog::warn("Renderer: failed to get shared handle[{}]: HRESULT {:#x}", i, static_cast<unsigned long>(hr));
            }
        }
    }
    return true;
}

void Renderer::draw_headless_and_publish(const PresentDecision& decision, const char* label) {
    int back = pick_free_buffer();
    cached_rtv_ = dbuf_.rtvs[back];
    draw_frame(decision);
    wait_gpu_and_swap(back, label);
    if (frame_callback_) frame_callback_();
    preview_drawn_ = true;
}

void Renderer::present_frame(const PresentDecision& decision) {
    spdlog::debug("[present_frame] mode={}", layout_.mode);
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    std::lock_guard<std::mutex> tex_lock(texture_mutex_);
    if (headless_) {
        draw_headless_and_publish(decision, "present_frame");
    } else {
        draw_frame(decision);
        d3d_device_->present(0);
        preview_drawn_ = true;
    }
}

void Renderer::redraw_layout() {
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    std::lock_guard<std::mutex> tex_lock(texture_mutex_);
    if (headless_) {
        draw_headless_and_publish(last_decision_, "redraw_layout");
    } else {
        draw_frame(last_decision_);
        d3d_device_->context()->Flush();
        if (frame_callback_) frame_callback_();
        preview_drawn_ = true;
    }
}

bool Renderer::has_any_frame(const PresentDecision& decision) {
    for (auto& f : decision.frames) {
        if (f.has_value()) return true;
    }
    return false;
}

int64_t Renderer::compute_frame_duration_us() const {
    // Use the minimum frame duration across all active tracks (= highest FPS).
    // This ensures step_backward moves in the finest granularity, so the fastest
    // track always advances exactly 1 frame; slower tracks hold until they have
    // a frame at the target PTS.
    int64_t min_dur = INT64_MAX;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) continue;
        auto frame = tracks_[i]->track_buffer->peek(0);
        if (frame.has_value() && frame->duration_us > 0) {
            min_dur = std::min(min_dur, frame->duration_us);
        }
    }
    if (min_dur != INT64_MAX && min_dur <= 100000) {
        return min_dur;
    }
    return 33333; // fallback ~30fps
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
    size_t count = 0;
    for (const auto& t : tracks_) { if (t) ++count; }
    return count;
}

int64_t Renderer::duration_us() const {
    return cached_duration_us_;
}

void Renderer::set_track_offset(int file_id, int64_t offset_us) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int slot = find_slot_by_file_id(file_id);
    if (slot < 0 || !tracks_[slot]) return;
    tracks_[slot]->offset_us = offset_us;
    render_sink_->set_track_offset(slot, offset_us);
    preview_drawn_ = false;
}

void Renderer::set_frame_callback(std::function<void()> cb) {
    frame_callback_ = std::move(cb);
}

ID3D11Texture2D* Renderer::shared_texture() const {
    return dbuf_.textures[dbuf_.front.load()].Get();
}

void Renderer::resize(int width, int height) {
    if (!headless_ || !d3d_device_) return;
    if (width <= 0 || height <= 0) return;
    pending_width_.store(width);
    pending_height_.store(height);
}

void Renderer::do_resize(int width, int height) {
    if (width == target_width_ && height == target_height_) return;

    spdlog::info("[Renderer] resize: {}x{} -> {}x{}", target_width_, target_height_, width, height);

    // Create new triple-buffered resources first, then swap under lock.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> new_textures[SharedBuffers::kCount];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> new_rtvs[SharedBuffers::kCount];
    HANDLE new_handles[SharedBuffers::kCount] = {};

    if (!create_shared_buffers(width, height, new_textures, new_rtvs, new_handles))
        return;

    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        std::lock_guard<std::mutex> tex_lock(texture_mutex_);

        PendingBuffers old;
        for (int i = 0; i < SharedBuffers::kCount; ++i) {
            old.textures[i] = std::move(dbuf_.textures[i]);
            old.handles[i] = dbuf_.handles[i];
        }
        old.expire_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        pending_destroy_.push_back(std::move(old));
        has_pending_destroy_.store(true);

        target_width_ = width;
        target_height_ = height;
        for (int i = 0; i < SharedBuffers::kCount; ++i) {
            dbuf_.textures[i] = std::move(new_textures[i]);
            dbuf_.rtvs[i] = std::move(new_rtvs[i]);
            dbuf_.handles[i] = new_handles[i];
        }
        dbuf_.front.store(0);

        draw_headless_and_publish(last_decision_, "resize");
    }

    spdlog::info("[Renderer] resize complete: {}x{}, handles=[{}, {}, {}]",
                 width, height,
                 reinterpret_cast<uintptr_t>(dbuf_.handles[0]),
                 reinterpret_cast<uintptr_t>(dbuf_.handles[1]),
                 reinterpret_cast<uintptr_t>(dbuf_.handles[2]));
}

void Renderer::render_loop() {
    // Raise Windows timer resolution from default ~15.6ms to 1ms,
    // so sleep_for(16ms) actually wakes up near 16ms instead of 31ms.
    timeBeginPeriod(1);
    spdlog::info("[Renderer] Render loop started (timer resolution: 1ms), tid={}", GetCurrentThreadId());

    // Periodic diagnostics — log buffer state every 2 seconds
    auto diag_time = std::chrono::steady_clock::now();
    int64_t diag_last_pts = 0;
    constexpr auto diag_interval = std::chrono::seconds(2);

    while (running_) {
        // Process pending resize (debounced — at most ~30Hz).
        {
            int pw = pending_width_.exchange(0);
            int ph = pending_height_.exchange(0);
            if (pw > 0 && ph > 0) {
                auto now = std::chrono::steady_clock::now();
                if (now - last_resize_time_ >= std::chrono::milliseconds(33)) {
                    do_resize(pw, ph);
                    last_resize_time_ = now;
                } else {
                    // Too soon — re-queue so the next iteration can pick it up.
                    // Write back only if no newer resize arrived in the meantime.
                    int expected = 0;
                    pending_width_.compare_exchange_strong(expected, pw);
                    expected = 0;
                    pending_height_.compare_exchange_strong(expected, ph);
                }
            }
        }

        // Clean up expired pending buffers from previous resizes.
        if (has_pending_destroy_.exchange(false)) {
            std::lock_guard<std::mutex> lock(texture_mutex_);
            auto now = std::chrono::steady_clock::now();
            pending_destroy_.erase(
                std::remove_if(pending_destroy_.begin(), pending_destroy_.end(),
                    [&](const PendingBuffers& pb) { return now >= pb.expire_time; }),
                pending_destroy_.end());
            if (!pending_destroy_.empty()) {
                has_pending_destroy_.store(true);
            }
        }

        // Snapshot playing_ under state_mutex_ to avoid torn read
        // when pause()/resume() modify it concurrently.
        bool playing_snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            playing_snapshot = playing_;
        }

        // Preroll: keep clock paused while any track is still buffering
        bool any_buffering = false;
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!tracks_[i]) continue;
            auto buf_state = tracks_[i]->track_buffer->state();
            if (buf_state == TrackState::Buffering || buf_state == TrackState::Empty) {
                any_buffering = true;
                break;
            }
        }

        // Detect Buffering → Ready transition: force preview redraw so the
        // newly-ready track's first frame appears on screen immediately
        // (even while paused — matches initialize() behavior).
        if (was_buffering_ && !any_buffering) {
            preview_drawn_ = false;
            last_decision_ = PresentDecision();  // Clear stale cached frames
            spdlog::info("[Renderer] Preroll transition complete, forcing preview redraw");
        }
        was_buffering_ = any_buffering;

        if (any_buffering && !clock_.is_paused()) {
            clock_.pause();
            spdlog::info("[Renderer] Preroll: clock PENDING, some track buffering, "
                         "(playing={})", playing_snapshot);
        } else if (!any_buffering && clock_.is_paused() && playing_snapshot) {
            clock_.resume();
            preview_drawn_ = false;
            spdlog::info("[Renderer] === Preroll COMPLETE: all tracks ready, clock resumed, "
                         "playing_={}, pts={:.3f}s)",
                         playing_snapshot, clock_.current_pts_us() / 1e6);
        }

        if (!playing_snapshot || clock_.is_paused()) {
            // While paused/prerolling, draw current frame if not yet drawn
            if (!preview_drawn_) {
                bool drawn = false;

                // Try cached last frame first (for layout changes while paused)
                if (has_any_frame(last_decision_)) {
                    present_frame(last_decision_);
                    drawn = true;
                    spdlog::debug("[Renderer] Paused frame (cached): pts={:.3f}s",
                                 [&]{
                                     for (auto& f : last_decision_.frames)
                                         if (f.has_value()) return f->pts_us / 1e6;
                                     return -1.0;
                                 }());
                }

                // No cached frame — try track buffer (initial preview)
                // Only draw when ALL active tracks have frames, to avoid
                // flashing black for tracks that haven't finished seeking.
                if (!drawn) {
                    PresentDecision preview;
                    preview.current_pts_us = 0;
                    preview.should_present = false;
                    bool all_active_have_frames = true;
                    for (size_t t = 0; t < kMaxTracks; ++t) {
                        if (!tracks_[t]) continue;
                        auto frame = tracks_[t]->track_buffer->peek(0);
                        if (frame.has_value()) {
                            preview.frames[t] = frame;
                        } else if (tracks_[t]->track_buffer->state() == TrackState::Ready) {
                            // Track is Ready but has no frames — past its duration (EOF).
                            // Don't block preview drawing for other tracks.
                        } else {
                            all_active_have_frames = false;
                        }
                    }
                    if (all_active_have_frames && has_any_frame(preview)) {
                        present_frame(preview);
                        last_decision_ = preview;
                        // Sync clock to the actual frame PTS so subsequent
                        // step_backward computes the correct seek target.
                        int ref = first_active_track();
                        if (ref >= 0 && preview.frames[ref].has_value()) {
                            clock_.seek(preview.frames[ref]->pts_us);
                        }
                        spdlog::info("[Renderer] Paused frame: pts={:.3f}s",
                                     ref >= 0 && preview.frames[ref].has_value()
                                     ? preview.frames[ref]->pts_us / 1e6 : -1.0);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto decision = render_sink_->evaluate();

        // Periodic diagnostics
        {
            auto now = std::chrono::steady_clock::now();
            if (now - diag_time >= diag_interval) {
                diag_time = now;
                int64_t pts = clock_.current_pts_us();
                int64_t pts_delta = pts - diag_last_pts;
                diag_last_pts = pts;
                for (size_t i = 0; i < kMaxTracks; ++i) {
                    if (!tracks_[i]) continue;
                    auto buf_count = tracks_[i]->track_buffer->total_count();
                    auto buf_cap = tracks_[i]->track_buffer->preroll_target();
                    auto buf_state = tracks_[i]->track_buffer->state();
                    spdlog::info("[diag] track[{}]: pts={:.3f}s delta={:.1f}ms "
                                 "buf={}/{} state={} playing={}",
                                 i, pts / 1e6, pts_delta / 1e3,
                                 buf_count, buf_cap,
                                 static_cast<int>(buf_state), playing_snapshot);
                }
            }
        }

        if (decision.should_present) {
            // Independent presentation: fill missing tracks from last decision
            // so each track always shows a frame (new or carried over).
            // Only carry over if the track is still within its effective window.
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!decision.frames[i].has_value() &&
                    last_decision_.frames[i].has_value() && tracks_[i]) {
                    int64_t effective_pts = decision.current_pts_us - tracks_[i]->offset_us;
                    int64_t track_dur = tracks_[i]->demux_thread->stats().duration_us;
                    if (effective_pts >= 0 && effective_pts <= track_dur) {
                        decision.frames[i] = last_decision_.frames[i];
                    }
                }
            }
            present_frame(decision);
            last_decision_ = decision;
        } else if (!preview_drawn_) {
            // No new frame but layout changed (e.g. zoom/pan during playback)
            if (has_any_frame(last_decision_)) {
                redraw_layout();
            }
        }

        // Frame-driven clock: when buffer is empty, clamp clock to the
        // end of the last presented frame so PTS doesn't run ahead.
        {
            bool buffer_empty = true;
            int64_t max_end_pts = 0;
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!tracks_[i]) continue;
                if (tracks_[i]->track_buffer->peek(0).has_value()) {
                    buffer_empty = false;
                    // No need to check further — one non-empty buffer is enough
                    break;
                }
                if (last_decision_.frames[i].has_value()) {
                    max_end_pts = std::max(max_end_pts,
                        last_decision_.frames[i]->pts_us +
                        last_decision_.frames[i]->duration_us);
                }
            }
            if (buffer_empty && max_end_pts > 0) {
                int64_t current = clock_.current_pts_us();
                if (current > max_end_pts) {
                    clock_.seek(max_end_pts);
                }
            }
        }

        // Deadline-based sleep: wake up at the exact wall time when the next
        // frame should be displayed.  This is drift-free because each sleep
        // targets an absolute PTS rather than an accumulated relative duration.
        {
            int64_t current_pts = clock_.current_pts_us();
            int64_t next_event_pts = INT64_MAX;

            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!tracks_[i]) continue;
                auto frame = tracks_[i]->track_buffer->peek(0);
                if (!frame.has_value()) continue;
                if (frame->pts_us > current_pts) {
                    // Future frame — wake when it should start
                    next_event_pts = std::min(next_event_pts, frame->pts_us);
                } else {
                    // Frame being displayed — wake when it expires
                    next_event_pts = std::min(next_event_pts,
                                              frame->pts_us + frame->duration_us);
                }
            }

            if (next_event_pts != INT64_MAX) {
                double spd = clock_.speed();
                if (spd > 0) {
                    int64_t pts_delta = next_event_pts - current_pts;
                    int64_t sleep_us = static_cast<int64_t>(pts_delta / spd);
                    if (sleep_us > 0) {
                        if (sleep_us > MAX_SLEEP_US) sleep_us = MAX_SLEEP_US;
                        std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
                    }
                }
            } else {
                // No frames available (buffer underflow) — short poll
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    // Flush any pending resize before exiting.
    {
        int pw = pending_width_.exchange(0);
        int ph = pending_height_.exchange(0);
        if (pw > 0 && ph > 0) do_resize(pw, ph);
    }

    spdlog::info("[Renderer] Render loop ended");
    timeEndPeriod(1);
}

void Renderer::draw_frame(const PresentDecision& decision) {
    auto* ctx = d3d_device_->context();

    // Get or create cached render target view
    if (!cached_rtv_) {
        if (headless_ && dbuf_.rtvs[0]) {
            int back = (dbuf_.front.load() + 1) % SharedBuffers::kCount;
            cached_rtv_ = dbuf_.rtvs[back];
        } else {
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
    }

    float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(cached_rtv_.Get(), clear_color);
    ctx->OMSetRenderTargets(1, cached_rtv_.GetAddressOf(), nullptr);

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
    ID3D11Buffer* vb = vertex_buffer_.Get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
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
            int mode;              // offset 0
            int track_count;       // offset 4
            float split_pos;       // offset 8
            float zoom_ratio;      // offset 12
            float canvas_width;    // offset 16
            float canvas_height;   // offset 20
            float view_offset[2];  // offset 24
            int order[4];          // offset 32
            float video_aspect[4]; // offset 48
            int nv12_mask;         // offset 64
            float _pad1[3];        // offset 68
            float nv12_uv_scale_y[4]; // offset 80
            float track_scale[4];  // offset 96: per-track scale for uniform pixel density

            // Precomputed per-track display params (offset 112-207)
            float display_offset_x[4];     // offset 112
            float display_offset_y[4];     // offset 128
            float inv_display_size_x[4];   // offset 144
            float inv_display_size_y[4];   // offset 160
            float view_offset_uv_x[4];    // offset 176
            float view_offset_uv_y[4];    // offset 192
        };
        static_assert(sizeof(Constants) == 208, "Constants must be 208 bytes");

        // Snapshot layout state atomically
        LayoutState snap;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            snap = layout_;
        }

        Constants cb = {};
        cb.mode = snap.mode;
        cb.split_pos = snap.split_pos;
        cb.zoom_ratio = snap.zoom_ratio;
        cb.canvas_width = static_cast<float>(target_width_);
        cb.canvas_height = static_cast<float>(target_height_);
        cb.view_offset[0] = snap.view_offset[0];
        cb.view_offset[1] = snap.view_offset[1];
        cb.nv12_mask = 0;
        for (int i = 0; i < 4; ++i) {
            cb.order[i] = snap.order[i];
        }
        int active_count = 0;
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!tracks_[i]) {
                cb.video_aspect[i] = 1.0f;
                cb.nv12_uv_scale_y[i] = 1.0f;
                continue;
            }
            ++active_count;
            cb.video_aspect[i] = tracks_[i]->video_aspect;
            if (decision.frames[i].has_value() && decision.frames[i]->is_nv12) {
                cb.nv12_mask |= (1 << static_cast<int>(i));
                const auto& frame = decision.frames[i].value();
                if (frame.texture_handle && frame.height > 0) {
                    auto* decode_tex = static_cast<ID3D11Texture2D*>(frame.texture_handle);
                    D3D11_TEXTURE2D_DESC tex_desc = {};
                    decode_tex->GetDesc(&tex_desc);
                    if (tex_desc.Height > 0 &&
                        tex_desc.Height != static_cast<UINT>(frame.height)) {
                        cb.nv12_uv_scale_y[i] =
                            static_cast<float>(frame.height) /
                            static_cast<float>(tex_desc.Height);
                        tracks_[i]->nv12_uv_scale_y = cb.nv12_uv_scale_y[i];
                    } else {
                        cb.nv12_uv_scale_y[i] = 1.0f;
                        tracks_[i]->nv12_uv_scale_y = 1.0f;
                    }
                } else {
                    cb.nv12_uv_scale_y[i] = tracks_[i]->nv12_uv_scale_y;
                }
            } else {
                cb.nv12_uv_scale_y[i] = tracks_[i]->nv12_uv_scale_y;
            }
        }
        cb.track_count = active_count;

        // Compute per-track scale for uniform pixel density across all tracks.
        // Find the reference track (highest resolution) and scale other tracks
        // so all videos share the same pixel density (video pixel -> screen pixel ratio).
        {
            int ref_idx = -1;
            int max_pixels = 0;
            for (int i = 0; i < 4; ++i) {
                if (!tracks_[i]) continue;
                int pixels = tracks_[i]->video_width * tracks_[i]->video_height;
                if (pixels > max_pixels) {
                    max_pixels = pixels;
                    ref_idx = i;
                }
            }
            if (ref_idx < 0) ref_idx = 0;

            // Slot dimensions depend on layout mode
            float slot_w = static_cast<float>(target_width_);
            float slot_h = static_cast<float>(target_height_);
            if (snap.mode != LAYOUT_SPLIT_SCREEN && active_count > 1) {
                slot_w /= static_cast<float>(active_count);
            }

            // Reference video density: min(slot_w / ref_w, slot_h / ref_h)
            float ref_density = 1.0f;
            if (tracks_[ref_idx]) {
                float ref_w = static_cast<float>(tracks_[ref_idx]->video_width);
                float ref_h = static_cast<float>(tracks_[ref_idx]->video_height);
                if (ref_w > 0.0f && ref_h > 0.0f) {
                    ref_density = std::min(slot_w / ref_w, slot_h / ref_h);
                }
            }

            for (int i = 0; i < 4; ++i) {
                if (!tracks_[i]) {
                    cb.track_scale[i] = 1.0f;
                    continue;
                }
                float tw = static_cast<float>(tracks_[i]->video_width);
                float th = static_cast<float>(tracks_[i]->video_height);
                float density = 1.0f;
                if (tw > 0.0f && th > 0.0f) {
                    density = std::min(slot_w / tw, slot_h / th);
                }
                cb.track_scale[i] = (density > 0.0f) ? ref_density / density : 1.0f;
            }
        }

        // Precompute per-track display constants (moves heavy math from pixel shader to CPU)
        {
            float slot_w = static_cast<float>(target_width_);
            float slot_h = static_cast<float>(target_height_);
            if (snap.mode != LAYOUT_SPLIT_SCREEN && active_count > 1) {
                slot_w /= static_cast<float>(active_count);
            }
            float slot_aspect = (slot_h > 0.0f) ? slot_w / slot_h : 1.0f;

            for (int i = 0; i < 4; ++i) {
                float video_aspect = cb.video_aspect[i];
                if (video_aspect <= 0.0f) video_aspect = slot_aspect;

                // Aspect-fit scale
                float fit_scale = (video_aspect > slot_aspect)
                    ? slot_aspect / video_aspect : 1.0f;
                fit_scale *= cb.track_scale[i];

                // Apply zoom
                float display_scale = fit_scale * snap.zoom_ratio;

                // Display size in slot UV space
                float ds_x = (slot_aspect > 0.0f)
                    ? video_aspect * display_scale / slot_aspect : display_scale;
                float ds_y = display_scale;

                // Display offset (centering)
                cb.display_offset_x[i] = (1.0f - ds_x) * 0.5f;
                cb.display_offset_y[i] = (1.0f - ds_y) * 0.5f;

                // Inverse display size (for fast division in shader)
                cb.inv_display_size_x[i] = (fabsf(ds_x) > 1e-4f) ? 1.0f / ds_x : 0.0f;
                cb.inv_display_size_y[i] = (fabsf(ds_y) > 1e-4f) ? 1.0f / ds_y : 0.0f;

                // View offset in video UV space
                float dp_x = ds_x * slot_w;
                float dp_y = ds_y * slot_h;
                cb.view_offset_uv_x[i] = (fabsf(dp_x) > 1e-4f) ? snap.view_offset[0] / dp_x : 0.0f;
                cb.view_offset_uv_y[i] = (fabsf(dp_y) > 1e-4f) ? snap.view_offset[1] / dp_y : 0.0f;
            }
        }
        ctx->UpdateSubresource(compiled_shader_.constant_buffer.Get(), 0, nullptr, &cb, 0, 0);
        ctx->PSSetConstantBuffers(0, 1, compiled_shader_.constant_buffer.GetAddressOf());
    }

    // Set sampler
    if (sampler_state_) {
        ID3D11SamplerState* sampler = sampler_state_.Get();
        ctx->PSSetSamplers(0, 1, &sampler);
    }

    // Set textures from frames
    ID3D11ShaderResourceView* srvs[4] = {};           // t0-t3: RGBA (sw) or full NV12 (hw)
    ID3D11ShaderResourceView* nv12_y_srvs[4] = {};    // t4-t7: NV12 Y plane
    ID3D11ShaderResourceView* nv12_uv_srvs[4] = {};   // t8-t11: NV12 UV plane

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!decision.frames[i].has_value() || !decision.frames[i]->texture_handle) continue;
        if (!tracks_[i]) continue;
        const auto& frame = decision.frames[i].value();
        auto& track = tracks_[i];

        if (frame.is_ref && frame.is_nv12) {
            // D3D11VA NV12 hardware decode: texture_handle is ID3D11Texture2D*
            // on the independent decode device. Open shared resource on render device
            // for cross-device SRV access.
            auto* decode_tex = static_cast<ID3D11Texture2D*>(frame.texture_handle);
            int array_idx = static_cast<int>(frame.texture_array_index);

            if (track->last_nv12_tex != decode_tex || track->last_nv12_idx != array_idx) {
                // Decode texture pointer changed — re-open shared resource on render device
                if (track->last_nv12_tex != decode_tex) {
                    track->render_nv12_tex.Reset();
                    track->nv12_y_srv.Reset();
                    track->nv12_uv_srv.Reset();

                    // Get DXGI shared handle from decode-side texture
                    Microsoft::WRL::ComPtr<IDXGIResource> dxgi_res;
                    HRESULT hr = decode_tex->QueryInterface(__uuidof(IDXGIResource), &dxgi_res);
                    if (FAILED(hr)) {
                        spdlog::error("[Renderer] Failed to QI IDXGIResource for track {}: {:#x}",
                                      i, static_cast<unsigned long>(hr));
                        track->last_nv12_tex = decode_tex;
                        track->last_nv12_idx = array_idx;
                        continue;
                    }

                    HANDLE shared_handle = nullptr;
                    hr = dxgi_res->GetSharedHandle(&shared_handle);
                    if (FAILED(hr)) {
                        spdlog::error("[Renderer] Failed to get shared handle for track {}: {:#x}",
                                      i, static_cast<unsigned long>(hr));
                        track->last_nv12_tex = decode_tex;
                        track->last_nv12_idx = array_idx;
                        continue;
                    }

                    // Open shared texture on render device
                    hr = d3d_device_->device()->OpenSharedResource(
                        shared_handle, __uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(track->render_nv12_tex.GetAddressOf()));
                    if (FAILED(hr)) {
                        spdlog::error("[Renderer] Failed to open shared NV12 texture for track {}: {:#x}",
                                      i, static_cast<unsigned long>(hr));
                        track->last_nv12_tex = decode_tex;
                        track->last_nv12_idx = array_idx;
                        continue;
                    }
                }

                // Create SRVs on the render-side shared texture
                ID3D11Texture2D* render_tex = track->render_nv12_tex.Get();
                D3D11_TEXTURE2D_DESC tex_desc = {};
                render_tex->GetDesc(&tex_desc);
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
                    render_tex, &y_desc, &track->nv12_y_srv);
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
                    render_tex, &uv_desc, &track->nv12_uv_srv);
                if (FAILED(hr)) {
                    spdlog::error("[Renderer] Failed to create NV12 UV SRV for track {}: {:#x}",
                                  i, static_cast<unsigned long>(hr));
                }

                track->last_nv12_tex = decode_tex;
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
            if (tracks_[i]) {
                is_cached = (srvs[i] == tracks_[i]->sw_srv.Get());
            }
            if (!is_cached) {
                srvs[i]->Release();
            }
        }
    }
}

// -- Layout control --
void Renderer::apply_layout(const LayoutState& state) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    layout_.mode = state.mode;
    layout_.split_pos = std::clamp(state.split_pos, 0.0f, 1.0f);
    layout_.zoom_ratio = std::clamp(state.zoom_ratio, 1.0f, 50.0f);
    layout_.view_offset[0] = state.view_offset[0];
    layout_.view_offset[1] = state.view_offset[1];

    // Translate file_id order → slot order for the shader
    for (int i = 0; i < 4; ++i) {
        file_id_order_[i] = state.order[i];
        int slot = find_slot_by_file_id(state.order[i]);
        layout_.order[i] = (slot >= 0) ? slot : 0;
    }

    // Trigger redraw — during playback, redraw_layout() handles this
    // without Flush() to avoid contention with D3D11VA decode threads.
    preview_drawn_ = false;
}

LayoutState Renderer::layout() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    LayoutState result = layout_;
    // Return file_id order (not slot order) to Flutter
    for (int i = 0; i < 4; ++i) {
        result.order[i] = file_id_order_[i];
    }
    return result;
}

// -- Dynamic track management --

int Renderer::first_active_track() const {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) return static_cast<int>(i);
    }
    return -1;
}

int Renderer::find_slot_by_file_id(int file_id) const {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i] && tracks_[i]->file_id == file_id)
            return static_cast<int>(i);
    }
    return -1;
}

int Renderer::find_empty_slot() const {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) return static_cast<int>(i);
    }
    return -1;
}

std::unique_ptr<Renderer::TrackPipeline> Renderer::create_pipeline(const std::string& path,
                                                                      bool hw_decode) {
    auto pipeline = std::make_unique<TrackPipeline>();
    pipeline->file_path = path;
    pipeline->seek_controller = std::make_unique<SeekController>();
    pipeline->packet_queue = std::make_unique<PacketQueue>(100);
    pipeline->track_buffer = std::make_unique<TrackBuffer>(8, 2);
    pipeline->demux_thread = std::make_unique<DemuxThread>(
        path, *pipeline->packet_queue, *pipeline->seek_controller);

    if (!pipeline->demux_thread->start()) {
        spdlog::error("Renderer: failed to start demux for {}", path);
        return nullptr;
    }

    const auto& stats = pipeline->demux_thread->stats();
    if (stats.video_stream_index < 0) {
        spdlog::error("Renderer: no video stream found in {}", path);
        pipeline->demux_thread->stop();
        return nullptr;
    }

    pipeline->video_width = stats.width;
    pipeline->video_height = stats.height;
    if (stats.width > 0 && stats.height > 0) {
        float sar = (stats.sar_den > 0)
            ? static_cast<float>(stats.sar_num) / static_cast<float>(stats.sar_den)
            : 1.0f;
        pipeline->video_aspect = (static_cast<float>(stats.width) / static_cast<float>(stats.height)) * sar;
    }

    pipeline->decode_thread = std::make_unique<DecodeThread>(
        *pipeline->packet_queue, *pipeline->track_buffer,
        stats.codec_params, stats.time_base);

    if (!pipeline->decode_thread->is_valid()) {
        spdlog::error("Renderer: decode thread init failed for {}", path);
        pipeline->demux_thread->stop();
        return nullptr;
    }

    pipeline->demux_thread->set_seek_callback(
        [dt = pipeline->decode_thread.get()](int64_t pts, SeekType type) {
            dt->notify_seek(pts, type);
        });

    if (hw_decode) {
        // Pass nullptr device — D3D11VA provider will create its own
        // independent D3D11 device for decoding. This avoids sharing the
        // render device's immediate context, which causes D3D11VA internal
        // state corruption on seek (C++ exception from D3D11 runtime).
        pipeline->decode_thread->enable_hardware_decode(nullptr, nullptr);
    }

    if (!pipeline->decode_thread->start()) {
        spdlog::error("Renderer: failed to start decode for {}", path);
        pipeline->demux_thread->stop();
        return nullptr;
    }

    return pipeline;
}

int Renderer::add_track(const std::string& video_path) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_) return -1;

    int slot = find_empty_slot();
    if (slot < 0) {
        spdlog::warn("Renderer::add_track: no empty slots");
        return -1;
    }

    // Pause playback to avoid render loop reading partially-initialized pipeline
    bool was_playing = playing_.load();
    if (was_playing) { clock_.pause(); playing_ = false; }

    auto pipeline = create_pipeline(video_path);
    if (!pipeline) {
        if (was_playing) { clock_.resume(); playing_ = true; }
        return -1;
    }

    // Register with render sink
    render_sink_->set_track(slot, pipeline->track_buffer.get());
    render_sink_->set_track_offset(slot, 0);

    // Update duration cache
    cached_duration_us_ = std::max(cached_duration_us_,
        pipeline->demux_thread->stats().duration_us);

    // Commit: install the pipeline
    tracks_[slot] = std::move(pipeline);
    tracks_[slot]->file_id = next_file_id_++;
    int new_file_id = tracks_[slot]->file_id;

    // Append new file_id to the order arrays
    for (int i = 0; i < 4; ++i) {
        if (file_id_order_[i] < 0) {
            file_id_order_[i] = new_file_id;
            layout_.order[i] = slot;
            break;
        }
    }

    // Seek new track to current clock position so evaluate() can find matching frames.
    // Without this, the new track starts from PTS=0 and evaluate() discards all its
    // frames as "expired" when the clock is elsewhere, causing both panels to show
    // the same old video.
    int64_t current_pts = clock_.current_pts_us();
    if (current_pts > 0) {
        auto& track = tracks_[slot];
        int64_t track_target = std::max(current_pts - track->offset_us, int64_t(0));
        track->decode_thread->set_decode_paused(true);
        track->track_buffer->set_state(TrackState::Flushing);
        track->track_buffer->clear_frames();
        track->packet_queue->flush();
        track->seek_controller->request_seek(track_target, SeekType::Keyframe);
        track->track_buffer->set_state(TrackState::Buffering);
        spdlog::info("Renderer::add_track: seeking slot={} to {:.3f}s (offset={:.3f}s)",
                     slot, track_target / 1e6, track->offset_us / 1e6);
    }

    // Force redraw, but keep already-presented frames from existing tracks so
    // they remain visible while the new track is still buffering/soft-decoding.
    preview_drawn_ = false;
    last_decision_.frames[slot] = std::nullopt;

    spdlog::info("Renderer::add_track: slot={} path={}", slot, video_path);
    return slot;
}

void Renderer::remove_track(int file_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int slot = find_slot_by_file_id(file_id);
    if (slot < 0) return;

    spdlog::info("Renderer::remove_track: file_id={}, slot={}", file_id, slot);

    // Pause playback
    bool was_playing = playing_.load();
    if (was_playing) { clock_.pause(); playing_ = false; }

    // Stop the pipeline
    auto& track = tracks_[slot];
    track->decode_thread->stop();
    track->demux_thread->stop();

    // Unregister from render sink
    render_sink_->set_track(slot, nullptr);

    // Release the pipeline
    track.reset();

    // Compact: shift tracks_[slot+1..] down to fill the gap
    for (size_t i = slot; i < kMaxTracks - 1; ++i) {
        if (!tracks_[i + 1]) break;  // No more tracks to compact
        tracks_[i] = std::move(tracks_[i + 1]);
        // Update render sink mapping (track buffer + offset)
        render_sink_->set_track(i, tracks_[i]->track_buffer.get());
        render_sink_->set_track_offset(i, tracks_[i]->offset_us);
        render_sink_->set_track(i + 1, nullptr);
    }

    // Compact last_decision_.frames the same way
    for (size_t i = slot; i < kMaxTracks - 1; ++i) {
        last_decision_.frames[i] = std::move(last_decision_.frames[i + 1]);
    }
    last_decision_.frames[kMaxTracks - 1] = std::nullopt;

    // Compact file_id_order_: remove the deleted file_id, shift remaining down
    for (int i = 0; i < 4; ++i) {
        if (file_id_order_[i] == file_id) {
            for (int j = i; j < 3; ++j) file_id_order_[j] = file_id_order_[j + 1];
            file_id_order_[3] = -1;
            break;
        }
    }

    // Re-translate file_id order → slot order after compact
    for (int i = 0; i < 4; ++i) {
        int s = find_slot_by_file_id(file_id_order_[i]);
        layout_.order[i] = (s >= 0) ? s : 0;
    }

    // Recalculate duration
    cached_duration_us_ = 0;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) {
            cached_duration_us_ = std::max(cached_duration_us_,
                tracks_[i]->demux_thread->stats().duration_us);
        }
    }

    preview_drawn_ = false;

    // If still have tracks and was playing, resume
    if (was_playing && first_active_track() >= 0) {
        clock_.resume();
        playing_ = true;
    }

    spdlog::info("Renderer::remove_track: file_id={}, slot={}, remaining={}", file_id, slot, track_count());
}

bool Renderer::has_track(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(kMaxTracks)) return false;
    return tracks_[slot] != nullptr;
}

std::pair<int, int> Renderer::track_dimensions(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(kMaxTracks) || !tracks_[slot]) {
        return {0, 0};
    }
    return {tracks_[slot]->video_width, tracks_[slot]->video_height};
}

std::vector<TrackInfo> Renderer::track_infos() const {
    std::vector<TrackInfo> infos;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) {
            infos.push_back({
                tracks_[i]->file_id,
                static_cast<int>(i),
                tracks_[i]->file_path,
                tracks_[i]->video_width,
                tracks_[i]->video_height,
                tracks_[i]->demux_thread ? tracks_[i]->demux_thread->stats().duration_us : 0
            });
        }
    }
    return infos;
}

std::vector<TrackPerfStats> Renderer::track_perf_stats() const {
    std::vector<TrackPerfStats> result;
    auto now = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(now - stats_start_time_).count();

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) continue;
        const auto& track = tracks_[i];
        auto snap = track->decode_thread->perf_counters().snapshot();

        TrackPerfStats s;
        s.slot = static_cast<int>(i);
        s.file_id = track->file_id;
        s.buffer_count = track->track_buffer->total_count();
        s.buffer_capacity = track->track_buffer->preroll_target();
        s.buffer_state = track->track_buffer->state();

        // Average decode time
        if (snap.frames_decoded > 0) {
            s.avg_decode_ms = static_cast<double>(snap.total_decode_us) /
                              static_cast<double>(snap.frames_decoded) / 1000.0;
        }
        s.max_decode_ms = static_cast<double>(snap.max_decode_us) / 1000.0;

        // FPS: delta frames / delta time since last snapshot
        auto& baseline = perf_baselines_[i];
        uint64_t delta_frames = snap.frames_decoded - baseline.frames;
        if (elapsed_s > 0.5) {
            s.fps = static_cast<double>(delta_frames) / elapsed_s;
            baseline.frames = snap.frames_decoded;
        }

        result.push_back(s);
    }

    // Reset shared timer once after all tracks are processed
    if (elapsed_s > 0.5) {
        stats_start_time_ = now;
    }
    return result;
}

} // namespace vr

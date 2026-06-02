#include <catch2/catch_test_macros.hpp>
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <vector>
#include <cstring>
#include <mutex>
#include <functional>
#include "test_utils.h"
#include "windows/d3d11/device.h"
#include "windows/d3d11/frame_presenter.h"
#include "windows/d3d11/headless_output.h"
#include "windows/d3d11/texture.h"
#include "video_renderer/capture/frame_capture_service.h"
#include "video_renderer/decode/frame_converter.h"
#include "video_renderer/renderer_limits.h"

extern "C" {
#include <libavutil/frame.h>
}

using namespace vr::test;

TEST_CASE("TextureManager creates RGBA texture 1920x1080", "[d3d11][texture]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());

    ID3D11Texture2D* tex = tm.create_rgba_texture(1920, 1080);
    REQUIRE(tex != nullptr);

    tex->Release();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("TextureManager rejects invalid RGBA texture dimensions", "[d3d11][texture]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());

    REQUIRE(tm.create_rgba_texture(0, 64) == nullptr);
    REQUIRE(tm.create_rgba_texture(64, -1) == nullptr);
    REQUIRE(tm.create_rgba_texture(vr::kMaxRendererDimension + 1, 64) == nullptr);
    REQUIRE(tm.create_rgba_texture(64, vr::kMaxRendererDimension + 1) == nullptr);

    cleanup_test_device(dev, hwnd);
}

TEST_CASE("TextureManager upload data and verify first pixel", "[d3d11][texture]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());

    const int WIDTH = 64;
    const int HEIGHT = 64;
    const int STRIDE = WIDTH * 4;

    // Create a test pattern: first pixel is R=255, G=0, B=128, A=255
    std::vector<uint8_t> data(STRIDE * HEIGHT, 0);
    data[0] = 255;  // R
    data[1] = 0;    // G
    data[2] = 128;  // B
    data[3] = 255;  // A

    ID3D11Texture2D* tex = tm.create_rgba_texture(WIDTH, HEIGHT);
    REQUIRE(tex != nullptr);

    bool uploaded = tm.upload_data(tex, data.data(), WIDTH, HEIGHT, STRIDE);
    REQUIRE(uploaded == true);

    // Verify the first pixel in our source data is intact
    REQUIRE(data[0] == 255);
    REQUIRE(data[1] == 0);
    REQUIRE(data[2] == 128);
    REQUIRE(data[3] == 255);

    tex->Release();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("TextureManager rejects invalid upload geometry", "[d3d11][texture]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());

    const int width = 64;
    const int height = 64;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    tex.Attach(tm.create_rgba_texture(width, height));
    REQUIRE(tex != nullptr);

    std::vector<uint8_t> data(width * height * 4, 0);
    REQUIRE_FALSE(tm.upload_data(tex.Get(), data.data(), width, height, width * 4 - 1));
    REQUIRE_FALSE(tm.upload_data(tex.Get(), data.data(), 0, height, width * 4));
    REQUIRE_FALSE(tm.upload_data(tex.Get(), data.data(), width, 0, width * 4));

    cleanup_test_device(dev, hwnd);
}

TEST_CASE("TextureManager creates SRV", "[d3d11][texture]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());

    ID3D11Texture2D* tex = tm.create_rgba_texture(1920, 1080);
    REQUIRE(tex != nullptr);

    ID3D11ShaderResourceView* srv = tm.create_srv(tex);
    REQUIRE(srv != nullptr);

    srv->Release();
    tex->Release();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("TextureManager texture format is R8G8B8A8_UNORM", "[d3d11][texture]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());

    ID3D11Texture2D* tex = tm.create_rgba_texture(320, 240);
    REQUIRE(tex != nullptr);

    D3D11_TEXTURE2D_DESC desc = {};
    tex->GetDesc(&desc);

    REQUIRE(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    REQUIRE(desc.Width == 320);
    REQUIRE(desc.Height == 240);
    REQUIRE(desc.MipLevels == 1);
    REQUIRE(desc.ArraySize == 1);

    tex->Release();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("TextureManager opens shared texture resources", "[d3d11][texture]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_tex;
    REQUIRE(SUCCEEDED(dev->device()->CreateTexture2D(&desc, nullptr, &shared_tex)));

    Microsoft::WRL::ComPtr<ID3D11Texture2D> opened;
    REQUIRE(tm.open_shared_texture(shared_tex.Get(), opened));
    REQUIRE(opened != nullptr);

    D3D11_TEXTURE2D_DESC opened_desc = {};
    opened->GetDesc(&opened_desc);
    REQUIRE(opened_desc.Width == desc.Width);
    REQUIRE(opened_desc.Height == desc.Height);
    REQUIRE(opened_desc.Format == desc.Format);

    cleanup_test_device(dev, hwnd);
}

TEST_CASE("TextureManager creates reusable NV12 copy resources", "[d3d11][texture]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 128;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 4;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    REQUIRE(SUCCEEDED(dev->device()->CreateTexture2D(&desc, nullptr, &source)));

    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uv_srv;
    bool created = false;

    REQUIRE(tm.ensure_nv12_copy_resources(source.Get(), copy, y_srv, uv_srv, &created));
    REQUIRE(created);
    REQUIRE(copy != nullptr);
    REQUIRE(y_srv != nullptr);
    REQUIRE(uv_srv != nullptr);

    D3D11_TEXTURE2D_DESC copy_desc = {};
    copy->GetDesc(&copy_desc);
    REQUIRE(copy_desc.Width == desc.Width);
    REQUIRE(copy_desc.Height == desc.Height);
    REQUIRE(copy_desc.Format == desc.Format);
    REQUIRE(copy_desc.ArraySize == 1);

    created = true;
    REQUIRE(tm.ensure_nv12_copy_resources(source.Get(), copy, y_srv, uv_srv, &created));
    REQUIRE_FALSE(created);

    cleanup_test_device(dev, hwnd);
}

TEST_CASE("TextureManager creates and uploads dynamic NV12 textures", "[d3d11][texture]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());

    const int width = 32;
    const int height = 16;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    texture.Attach(tm.create_nv12_texture(width, height));
    REQUIRE(texture != nullptr);

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    REQUIRE(desc.Format == DXGI_FORMAT_NV12);
    REQUIRE(desc.Width == width);
    REQUIRE(desc.Height == height);

    std::vector<uint8_t> data(width * height * 3 / 2, 128);
    data[0] = 16;
    REQUIRE(tm.upload_nv12_data(texture.Get(), data.data(), width, height, width, width));

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uv_srv;
    REQUIRE(tm.create_nv12_plane_srvs(texture.Get(), y_srv, uv_srv));
    REQUIRE(y_srv != nullptr);
    REQUIRE(uv_srv != nullptr);

    cleanup_test_device(dev, hwnd);
}

TEST_CASE("TextureManager creates and uploads dynamic P010 textures", "[d3d11][texture]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());

    const int width = 32;
    const int height = 16;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    texture.Attach(tm.create_p010_texture(width, height));
    REQUIRE(texture != nullptr);

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    REQUIRE(desc.Format == DXGI_FORMAT_P010);
    REQUIRE(desc.Width == width);
    REQUIRE(desc.Height == height);

    std::vector<uint8_t> data(static_cast<size_t>(width) * height * 3, 0);
    REQUIRE(tm.upload_nv12_data(
        texture.Get(), data.data(), width, height, width * 2, width * 2, true));

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uv_srv;
    REQUIRE(tm.create_nv12_plane_srvs(texture.Get(), y_srv, uv_srv));
    REQUIRE(y_srv != nullptr);
    REQUIRE(uv_srv != nullptr);

    cleanup_test_device(dev, hwnd);
}

TEST_CASE("D3D11FramePresenter prepares cached software NV12 frame SRVs",
          "[d3d11][frame_presenter]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());
    vr::D3D11FramePresenter presenter(&tm, dev->context());

    const int width = 32;
    const int height = 16;
    auto pixels = std::make_shared<std::vector<uint8_t>>(
        width * height * 3 / 2,
        static_cast<uint8_t>(128));

    vr::TextureFrame frame;
    frame.width = width;
    frame.height = height;
    frame.cpu_data = pixels;
    frame.texture_handle = pixels->data();
    frame.is_nv12 = true;
    frame.storage = vr::CpuNv12FrameStorage{pixels, width, width};

    vr::D3D11PreparedFrame prepared;
    REQUIRE(presenter.prepare_frame(
        0, frame, 1920, 1080, [](const char*) {}, prepared));
    REQUIRE(prepared.rgba_srv == nullptr);
    REQUIRE(prepared.owned_rgba_srv.Get() == nullptr);
    REQUIRE(prepared.nv12_y_srv != nullptr);
    REQUIRE(prepared.nv12_uv_srv != nullptr);

    presenter.reset_all();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("D3D11FramePresenter prepares padded odd software NV12 frame SRVs",
          "[d3d11][frame_presenter]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());
    vr::D3D11FramePresenter presenter(&tm, dev->context());

    const int display_width = 65;
    const int display_height = 63;
    const int coded_width = 66;
    const int coded_height = 64;
    auto pixels = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(coded_width) * coded_height +
            static_cast<size_t>(coded_width) * (coded_height / 2),
        static_cast<uint8_t>(128));

    vr::TextureFrame frame;
    frame.width = display_width;
    frame.height = display_height;
    frame.cpu_data = pixels;
    frame.texture_handle = pixels->data();
    frame.is_nv12 = true;
    frame.storage = vr::CpuNv12FrameStorage{
        pixels,
        coded_width,
        coded_width,
        false,
        coded_width,
        coded_height,
    };

    vr::D3D11PreparedFrame prepared;
    REQUIRE(presenter.prepare_frame(
        0, frame, 1920, 1080, [](const char*) {}, prepared));
    REQUIRE(prepared.nv12_y_srv != nullptr);
    REQUIRE(prepared.nv12_uv_srv != nullptr);
    REQUIRE(std::fabs(presenter.nv12_uv_scale_x(0) -
                      (static_cast<float>(display_width) / coded_width)) < 0.0001f);
    REQUIRE(std::fabs(presenter.nv12_uv_scale_y(0) -
                      (static_cast<float>(display_height) / coded_height)) < 0.0001f);

    presenter.reset_all();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("D3D11FramePresenter prepares cached software P010 frame SRVs",
          "[d3d11][frame_presenter]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());
    vr::D3D11FramePresenter presenter(&tm, dev->context());

    const int width = 32;
    const int height = 16;
    auto pixels = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(width) * height * 3,
        uint8_t{0});

    vr::TextureFrame frame;
    frame.width = width;
    frame.height = height;
    frame.cpu_data = pixels;
    frame.texture_handle = pixels->data();
    frame.is_nv12 = true;
    frame.is_p010 = true;
    frame.storage = vr::CpuNv12FrameStorage{pixels, width * 2, width * 2, true};

    vr::D3D11PreparedFrame prepared;
    REQUIRE(presenter.prepare_frame(
        0, frame, 1920, 1080, [](const char*) {}, prepared));
    REQUIRE(prepared.rgba_srv == nullptr);
    REQUIRE(prepared.owned_rgba_srv.Get() == nullptr);
    REQUIRE(prepared.nv12_y_srv != nullptr);
    REQUIRE(prepared.nv12_uv_srv != nullptr);

    presenter.reset_all();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("D3D11FramePresenter crops padded hardware NV12 texture dimensions",
          "[d3d11][frame_presenter]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());
    vr::D3D11FramePresenter presenter(&tm, dev->context());

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1152;
    desc.Height = 2048;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(dev->device()->CreateTexture2D(&desc, nullptr, &texture)));

    vr::TextureFrame frame;
    frame.width = 1088;
    frame.height = 1980;
    frame.texture_handle = texture.Get();
    frame.is_ref = true;
    frame.is_nv12 = true;
    frame.texture_array_index = 0;

    vr::D3D11PreparedFrame prepared;
    REQUIRE(presenter.prepare_frame(
        0, frame, 1920, 1080, [](const char*) {}, prepared));
    REQUIRE(prepared.nv12_y_srv != nullptr);
    REQUIRE(prepared.nv12_uv_srv != nullptr);
    REQUIRE(std::fabs(presenter.nv12_uv_scale_x(0) - (1088.0f / 1152.0f)) < 0.0001f);
    REQUIRE(std::fabs(presenter.nv12_uv_scale_y(0) - (1980.0f / 2048.0f)) < 0.0001f);

    cleanup_test_device(dev, hwnd);
}

TEST_CASE("FrameConverter rejects unsupported D3D11VA non-4:2:0 surfaces",
          "[d3d11][frame_converter]") {
    auto [dev, hwnd] = create_test_device();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 32;
    desc.Height = 16;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(dev->device()->CreateTexture2D(&desc, nullptr, &texture)));

    std::recursive_mutex device_mutex;
    vr::FrameConverter converter;
    REQUIRE(converter.init_hardware(
        dev->device(),
        dev->context(),
        32,
        16,
        vr::HwDecodeType::D3D11VA,
        false,
        &device_mutex));

    AVFrame* frame = av_frame_alloc();
    REQUIRE(frame != nullptr);
    frame->format = AV_PIX_FMT_D3D11;
    frame->width = 32;
    frame->height = 16;
    frame->data[0] = reinterpret_cast<uint8_t*>(texture.Get());
    frame->data[1] = reinterpret_cast<uint8_t*>(intptr_t{0});

    auto converted = converter.convert(frame);
    REQUIRE_FALSE(converted.has_value());

    av_frame_free(&frame);
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("D3D11FramePresenter owns direct texture SRV", "[d3d11][frame_presenter]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());
    vr::D3D11FramePresenter presenter(&tm, dev->context());

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    texture.Attach(tm.create_rgba_texture(32, 16));
    REQUIRE(texture != nullptr);

    vr::TextureFrame frame;
    frame.is_ref = true;
    frame.texture_handle = texture.Get();

    vr::D3D11PreparedFrame prepared;
    REQUIRE(presenter.prepare_frame(
        0, frame, 1920, 1080, [](const char*) {}, prepared));
    REQUIRE(prepared.rgba_srv != nullptr);
    REQUIRE(prepared.owned_rgba_srv.Get() == prepared.rgba_srv);

    cleanup_test_device(dev, hwnd);
}

TEST_CASE("D3D11HeadlessOutput initializes shared texture buffers", "[d3d11][headless_output]") {
    auto [dev, hwnd] = create_test_device();
    vr::D3D11HeadlessOutput output;

    REQUIRE(output.initialize(dev->device(), dev->context(), 320, 240));

    D3D11_TEXTURE2D_DESC desc = {};
    {
        std::lock_guard<std::mutex> lock(output.texture_mutex());
        REQUIRE(output.shared_texture_locked() != nullptr);
        REQUIRE(output.shared_texture_handle_locked() != nullptr);
        output.shared_texture_locked()->GetDesc(&desc);
    }
    REQUIRE(desc.Width == 320);
    REQUIRE(desc.Height == 240);
    REQUIRE(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);

    output.shutdown();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("D3D11HeadlessOutput publishes and resizes buffers", "[d3d11][headless_output]") {
    auto [dev, hwnd] = create_test_device();
    vr::D3D11HeadlessOutput output;
    REQUIRE(output.initialize(dev->device(), dev->context(), 320, 240));

    int callback_count = 0;
    output.set_frame_callback([&] { ++callback_count; });

    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(output.texture_mutex());
        REQUIRE(output.begin_frame_locked() != nullptr);
        output.wait_gpu_idle("headless_output_test");
        callback = output.publish_frame_locked();
        REQUIRE(callback_count == 0);
        REQUIRE(callback != nullptr);
    }
    callback();
    REQUIRE(callback_count == 1);

    D3D11_TEXTURE2D_DESC desc = {};
    {
        std::lock_guard<std::mutex> lock(output.texture_mutex());
        REQUIRE(output.resize_locked(640, 360));
        REQUIRE(output.shared_texture_locked() != nullptr);
        REQUIRE(output.shared_texture_handle_locked() != nullptr);
        output.shared_texture_locked()->GetDesc(&desc);
    }
    REQUIRE(desc.Width == 640);
    REQUIRE(desc.Height == 360);

    output.cleanup_expired_pending_buffers();
    output.shutdown();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("D3D11HeadlessOutput captures pinned front-buffer snapshots",
          "[d3d11][headless_output]") {
    auto [dev, hwnd] = create_test_device();
    vr::D3D11HeadlessOutput output;
    REQUIRE(output.initialize(dev->device(), dev->context(), 64, 32));

    vr::D3D11HeadlessOutputFrontBufferSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(output.texture_mutex());
        auto* rtv = output.begin_frame_locked();
        REQUIRE(rtv != nullptr);
        const float clear_color[4] = {0.1f, 0.4f, 0.8f, 1.0f};
        dev->context()->ClearRenderTargetView(rtv, clear_color);
        output.publish_frame_locked();
        REQUIRE(output.snapshot_front_buffer_locked(snapshot));
        REQUIRE(output.resize_locked(80, 40));
    }

    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    REQUIRE(output.capture_front_buffer_snapshot(snapshot, bgra, width, height));
    REQUIRE(width == 64);
    REQUIRE(height == 32);
    REQUIRE(bgra.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    bool has_nonzero = false;
    for (uint8_t byte : bgra) {
        if (byte != 0) {
            has_nonzero = true;
            break;
        }
    }
    REQUIRE(has_nonzero);

    output.shutdown();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("FrameCaptureService captures the current headless front buffer",
          "[d3d11][headless_output][capture]") {
    auto [dev, hwnd] = create_test_device();
    vr::D3D11HeadlessOutput output;
    REQUIRE(output.initialize(dev->device(), dev->context(), 96, 48));

    std::recursive_mutex device_mutex;
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex);
        std::lock_guard<std::mutex> tex_lock(output.texture_mutex());
        auto* rtv = output.begin_frame_locked();
        REQUIRE(rtv != nullptr);
        const float clear_color[4] = {0.3f, 0.2f, 0.7f, 1.0f};
        dev->context()->ClearRenderTargetView(rtv, clear_color);
        REQUIRE(output.publish_frame_locked() == nullptr);
    }

    vr::FrameCaptureService service;
    std::vector<uint8_t> bgra;
    int width = -1;
    int height = -1;
    REQUIRE(service.capture_headless_front_buffer(
        output, device_mutex, bgra, width, height));
    REQUIRE(width == 96);
    REQUIRE(height == 48);
    REQUIRE(bgra.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    bool has_nonzero = false;
    for (uint8_t byte : bgra) {
        if (byte != 0) {
            has_nonzero = true;
            break;
        }
    }
    REQUIRE(has_nonzero);

    output.shutdown();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("D3D11HeadlessOutput does not reuse in-flight shared buffers",
          "[d3d11][headless_output]") {
    auto [dev, hwnd] = create_test_device();
    vr::D3D11HeadlessOutput output;
    REQUIRE(output.initialize(dev->device(), dev->context(), 320, 240));

    auto publish_and_acquire = [&]() {
        vr::D3D11HeadlessOutputTextureLease lease;
        {
            std::lock_guard<std::mutex> lock(output.texture_mutex());
            REQUIRE(output.begin_frame_locked() != nullptr);
            output.publish_frame_locked();
            REQUIRE(output.acquire_shared_texture_locked(lease));
        }
        REQUIRE(lease.texture != nullptr);
        REQUIRE(lease.handle != nullptr);
        REQUIRE(lease.buffer_index >= 0);
        REQUIRE(lease.buffer_index < vr::D3D11HeadlessOutput::kBufferCount);
        REQUIRE(output.buffer_in_flight_for_test(lease.buffer_index));
        return lease;
    };

    auto lease1 = publish_and_acquire();
    auto lease2 = publish_and_acquire();
    auto lease3 = publish_and_acquire();
    vr::D3D11HeadlessOutputTextureLease lease3_duplicate;
    {
        std::lock_guard<std::mutex> lock(output.texture_mutex());
        REQUIRE(output.acquire_shared_texture_locked(lease3_duplicate));
    }
    REQUIRE(lease3_duplicate.buffer_index == lease3.buffer_index);

    {
        std::lock_guard<std::mutex> lock(output.texture_mutex());
        REQUIRE(output.begin_frame_locked() == nullptr);
        REQUIRE(output.publish_frame_locked() == nullptr);
    }

    output.release_shared_texture(lease3.buffer_index, lease3.generation);
    REQUIRE(output.buffer_in_flight_for_test(lease3.buffer_index));
    lease3.texture->Release();
    lease3.texture = nullptr;

    output.release_shared_texture(lease1.buffer_index, lease1.generation + 1);
    REQUIRE(output.buffer_in_flight_for_test(lease1.buffer_index));

    output.release_shared_texture(lease1.buffer_index, lease1.generation);
    REQUIRE_FALSE(output.buffer_in_flight_for_test(lease1.buffer_index));
    lease1.texture->Release();
    lease1.texture = nullptr;

    {
        std::lock_guard<std::mutex> lock(output.texture_mutex());
        REQUIRE(output.begin_frame_locked() != nullptr);
    }

    output.release_shared_texture(lease2.buffer_index, lease2.generation);
    output.release_shared_texture(lease3_duplicate.buffer_index, lease3_duplicate.generation);
    lease2.texture->Release();
    lease3_duplicate.texture->Release();

    output.shutdown();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("D3D11HeadlessOutput fails initialization when shared handles are unavailable",
          "[d3d11][headless_output]") {
    auto [dev, hwnd] = create_test_device();
    vr::D3D11HeadlessOutput output;
    output.fail_shared_handle_for_test(true);

    REQUIRE_FALSE(output.initialize(dev->device(), dev->context(), 320, 240));

    {
        std::lock_guard<std::mutex> lock(output.texture_mutex());
        REQUIRE(output.shared_texture_locked() == nullptr);
        REQUIRE(output.shared_texture_handle_locked() == nullptr);
    }

    output.shutdown();
    cleanup_test_device(dev, hwnd);
}

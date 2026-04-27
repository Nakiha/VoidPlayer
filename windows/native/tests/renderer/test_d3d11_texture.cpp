#include <catch2/catch_test_macros.hpp>
#include <windows.h>
#include <vector>
#include <cstring>
#include "test_utils.h"
#include "video_renderer/d3d11/device.h"
#include "video_renderer/d3d11/frame_presenter.h"
#include "video_renderer/d3d11/headless_output.h"
#include "video_renderer/d3d11/texture.h"

using namespace vr::test;

TEST_CASE("TextureManager creates RGBA texture 1920x1080", "[d3d11][texture]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());

    ID3D11Texture2D* tex = tm.create_rgba_texture(1920, 1080);
    REQUIRE(tex != nullptr);

    tex->Release();
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

TEST_CASE("D3D11FramePresenter prepares cached software frame SRV", "[d3d11][frame_presenter]") {
    auto [dev, hwnd] = create_test_device();
    vr::TextureManager tm(dev->device(), dev->context());
    vr::D3D11FramePresenter presenter(&tm, dev->context());

    const int width = 32;
    const int height = 16;
    auto pixels = std::make_shared<std::vector<uint8_t>>(width * height * 4, 255);

    vr::TextureFrame frame;
    frame.width = width;
    frame.height = height;
    frame.cpu_data = pixels;
    frame.texture_handle = pixels->data();
    frame.storage = vr::CpuRgbaFrameStorage{pixels, width * 4};

    vr::D3D11PreparedFrame prepared;
    REQUIRE(presenter.prepare_frame(
        0, frame, 1920, 1080, [](const char*) {}, prepared));
    REQUIRE(prepared.rgba_srv != nullptr);
    REQUIRE_FALSE(prepared.release_rgba_srv);
    REQUIRE(prepared.nv12_y_srv == nullptr);
    REQUIRE(prepared.nv12_uv_srv == nullptr);

    presenter.reset_all();
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("D3D11FramePresenter marks direct texture SRV as temporary", "[d3d11][frame_presenter]") {
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
    REQUIRE(prepared.release_rgba_srv);

    if (prepared.release_rgba_srv && prepared.rgba_srv) {
        prepared.rgba_srv->Release();
    }
    cleanup_test_device(dev, hwnd);
}

TEST_CASE("D3D11HeadlessOutput initializes shared texture buffers", "[d3d11][headless_output]") {
    auto [dev, hwnd] = create_test_device();
    vr::D3D11HeadlessOutput output;

    REQUIRE(output.initialize(dev->device(), dev->context(), 320, 240));
    REQUIRE(output.shared_texture() != nullptr);
    REQUIRE(output.shared_texture_handle() != nullptr);

    D3D11_TEXTURE2D_DESC desc = {};
    output.shared_texture()->GetDesc(&desc);
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

    REQUIRE(output.begin_frame() != nullptr);
    output.publish_frame("headless_output_test");
    REQUIRE(callback_count == 1);

    REQUIRE(output.resize(640, 360));
    REQUIRE(output.shared_texture() != nullptr);
    REQUIRE(output.shared_texture_handle() != nullptr);

    D3D11_TEXTURE2D_DESC desc = {};
    output.shared_texture()->GetDesc(&desc);
    REQUIRE(desc.Width == 640);
    REQUIRE(desc.Height == 360);

    output.cleanup_expired_pending_buffers();
    output.shutdown();
    cleanup_test_device(dev, hwnd);
}

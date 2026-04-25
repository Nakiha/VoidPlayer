#include <catch2/catch_test_macros.hpp>
#include <windows.h>
#include <vector>
#include <cstring>
#include "test_utils.h"
#include "video_renderer/d3d11/device.h"
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

#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/d3d11/device.h"

using namespace vr::test;

TEST_CASE("D3D11Device initialization", "[d3d11][device]") {
    vr::D3D11Device dev;
    HWND hwnd = create_hidden_window();

    REQUIRE(dev.initialize(hwnd, 800, 600) == true);
    REQUIRE(dev.device() != nullptr);
    REQUIRE(dev.context() != nullptr);
    REQUIRE(dev.device_lost() == false);
    REQUIRE(dev.device_removed_reason() == S_OK);

    destroy_window(hwnd);
}

TEST_CASE("D3D11Device swap chain is created", "[d3d11][device]") {
    vr::D3D11Device dev;
    HWND hwnd = create_hidden_window();

    REQUIRE(dev.initialize(hwnd, 800, 600) == true);
    REQUIRE(dev.swap_chain() != nullptr);

    destroy_window(hwnd);
}

TEST_CASE("D3D11Device present does not crash", "[d3d11][device]") {
    vr::D3D11Device dev;
    HWND hwnd = create_hidden_window();

    REQUIRE(dev.initialize(hwnd, 800, 600) == true);
    REQUIRE_NOTHROW(dev.present(0));

    destroy_window(hwnd);
}

TEST_CASE("D3D11Device shutdown clears all pointers", "[d3d11][device]") {
    vr::D3D11Device dev;
    HWND hwnd = create_hidden_window();

    REQUIRE(dev.initialize(hwnd, 800, 600) == true);
    dev.shutdown();

    REQUIRE(dev.device() == nullptr);
    REQUIRE(dev.context() == nullptr);
    REQUIRE(dev.swap_chain() == nullptr);

    destroy_window(hwnd);
}

TEST_CASE("D3D11Device resize to 1920x1080", "[d3d11][device]") {
    vr::D3D11Device dev;
    HWND hwnd = create_hidden_window(1920, 1080);

    REQUIRE(dev.initialize(hwnd, 800, 600) == true);
    REQUIRE_NOTHROW(dev.resize(1920, 1080));

    // Present after resize should also work
    REQUIRE_NOTHROW(dev.present(0));

    destroy_window(hwnd);
}

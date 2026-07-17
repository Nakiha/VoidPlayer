#include <catch2/catch_test_macros.hpp>

#include "windows/presentation/windows_compositor_viewport_handoff.h"

TEST_CASE("Windows retained handoff aligns horizontal sidebar target geometry",
          "[windows_presentation][windows_compositor_handoff]") {
  vr::WindowsCompositorViewportRect viewport{0, 70, 1894, 867, 1894, 1175};

  REQUIRE(vr::synchronize_retained_horizontal_viewport_handoff(
      viewport, 1299, 867));
  REQUIRE(viewport.left == 0);
  REQUIRE(viewport.top == 70);
  REQUIRE(viewport.width == 1299);
  REQUIRE(viewport.height == 867);
  REQUIRE(viewport.surface_width == 1894);
  REQUIRE(viewport.surface_height == 1175);

  REQUIRE(vr::synchronize_retained_horizontal_viewport_handoff(
      viewport, 1894, 867));
  REQUIRE(viewport.width == 1894);
}

TEST_CASE(
    "Windows retained handoff leaves non-horizontal geometry authoritative",
    "[windows_presentation][windows_compositor_handoff]") {
  vr::WindowsCompositorViewportRect viewport{0, 70, 1299, 867, 1894, 1175};

  REQUIRE_FALSE(vr::synchronize_retained_horizontal_viewport_handoff(
      viewport, 1894, 1175));
  REQUIRE_FALSE(vr::synchronize_retained_horizontal_viewport_handoff(
      viewport, 2000, 867));
  REQUIRE(viewport.width == 1299);
  REQUIRE(viewport.height == 867);
}

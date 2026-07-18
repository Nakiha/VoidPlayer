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

TEST_CASE("Windows retired targets require successful compositor grace",
          "[windows_presentation][windows_compositor_handoff]") {
  vr::WindowsRetiredTargetReleaseGate gate;

  REQUIRE_FALSE(gate.armed());
  REQUIRE_FALSE(gate.note_completion(1, true));

  gate.arm(10);
  REQUIRE(gate.armed());
  REQUIRE(gate.successful_composites_remaining() == 4);

  REQUIRE_FALSE(gate.note_completion(9, true));
  REQUIRE_FALSE(gate.note_completion(10, false));
  REQUIRE(gate.successful_composites_remaining() == 4);

  REQUIRE_FALSE(gate.note_completion(10, true));
  REQUIRE_FALSE(gate.note_completion(10, true));
  REQUIRE_FALSE(gate.note_completion(11, true));
  REQUIRE(gate.successful_composites_remaining() == 1);
  REQUIRE(gate.note_completion(12, true));
  REQUIRE_FALSE(gate.armed());
  REQUIRE(gate.successful_composites_remaining() == 0);
}

TEST_CASE("Windows retired target grace restarts for a newer ring",
          "[windows_presentation][windows_compositor_handoff]") {
  vr::WindowsRetiredTargetReleaseGate gate;

  gate.arm(20);
  REQUIRE_FALSE(gate.note_completion(20, true));
  REQUIRE(gate.successful_composites_remaining() == 3);

  gate.reset();
  gate.arm(30);
  REQUIRE_FALSE(gate.note_completion(29, true));
  REQUIRE(gate.successful_composites_remaining() == 4);
  for (uint64_t serial = 30; serial < 33; ++serial) {
    REQUIRE_FALSE(gate.note_completion(serial, true));
  }
  REQUIRE(gate.note_completion(33, true));
}

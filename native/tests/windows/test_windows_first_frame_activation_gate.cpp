#include <catch2/catch_test_macros.hpp>

#include "windows/presentation/windows_first_frame_activation_gate.h"

TEST_CASE("Windows first frame waits for policy viewport and current target",
          "[windows_presentation][windows_first_frame]") {
  vr::WindowsFirstFrameActivationGate gate;
  const auto session = gate.begin_session();

  REQUIRE(gate.awaiting_first_frame(session));
  REQUIRE_FALSE(gate.accept_present(session, true, true));

  REQUIRE(gate.mark_policy_ready(session));
  REQUIRE_FALSE(gate.accept_present(session, true, true));

  REQUIRE(gate.commit_initial_viewport(session));
  REQUIRE_FALSE(gate.accept_present(session, false, true));
  REQUIRE_FALSE(gate.accept_present(session, true, false));
  REQUIRE(gate.accept_present(session, true, true));
  REQUIRE(gate.active(session));
  REQUIRE_FALSE(gate.accept_present(session, true, true));
}

TEST_CASE("Windows first frame rejects retired player callbacks",
          "[windows_presentation][windows_first_frame]") {
  vr::WindowsFirstFrameActivationGate gate;
  const auto retired = gate.begin_session();
  REQUIRE(gate.mark_policy_ready(retired));
  REQUIRE(gate.commit_initial_viewport(retired));

  const auto current = gate.begin_session();
  REQUIRE_FALSE(gate.accept_present(retired, true, true));
  REQUIRE_FALSE(gate.active(retired));

  REQUIRE(gate.mark_policy_ready(current));
  REQUIRE(gate.commit_initial_viewport(current));
  REQUIRE(gate.accept_present(current, true, true));
  REQUIRE(gate.active(current));

  gate.cancel_session();
  REQUIRE_FALSE(gate.active(current));
  REQUIRE_FALSE(gate.accept_present(current, true, true));
}

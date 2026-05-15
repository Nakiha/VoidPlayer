#include "../../../windows/runner/native_player_registry.h"

#include <catch2/catch_test_macros.hpp>

namespace {

std::shared_ptr<vr::NativePlayer> make_dummy_player() {
    return std::shared_ptr<vr::NativePlayer>(
        reinterpret_cast<vr::NativePlayer*>(0x1),
        [](vr::NativePlayer*) {});
}

} // namespace

TEST_CASE("NativeDiagnosticsSession pins only its scoped player") {
    auto session = std::make_shared<NativeDiagnosticsSession>();
    REQUIRE_FALSE(session->PinPlayer());

    auto player = make_dummy_player();
    session->PublishPlayer(player);
    REQUIRE(session->PinPlayer().get() == player.get());

    session->ClearPlayer();
    REQUIRE_FALSE(session->PinPlayer());
}

TEST_CASE("NativeDiagnosticsSession drops expired players") {
    auto session = std::make_shared<NativeDiagnosticsSession>();
    {
        auto player = make_dummy_player();
        session->PublishPlayer(player);
        REQUIRE(session->PinPlayer().get() == player.get());
    }
    REQUIRE_FALSE(session->PinPlayer());
}

TEST_CASE("NativeDiagnosticsSessionRegistry clears only the owning session") {
    auto& registry = GlobalNativeDiagnosticsSessionRegistry();
    auto first = std::make_shared<NativeDiagnosticsSession>();
    auto second = std::make_shared<NativeDiagnosticsSession>();

    registry.Clear(first);
    registry.Clear(second);

    registry.Publish(first);
    REQUIRE(registry.PinSession() == first);

    registry.Clear(second);
    REQUIRE(registry.PinSession() == first);

    registry.Publish(second);
    REQUIRE(registry.PinSession() == second);

    registry.Clear(first);
    REQUIRE(registry.PinSession() == second);

    registry.Clear(second);
    REQUIRE_FALSE(registry.PinSession());
}

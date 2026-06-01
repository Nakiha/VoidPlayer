#include "video_renderer/decode/decode_exact_seek_reorder.h"

#include <iostream>

namespace {

int fail(const char* message) {
    std::cerr << message << "\n";
    return 1;
}

} // namespace

int main() {
    int drained = 0;
    int published = 0;
    const auto eof_result = vr::handle_exact_seek_reorder_after_receive(
        vr::DecodeExactSeekReorderState{
            true,
            2,
            true,
            0,
            false,
            false,
        },
        vr::DecodeExactSeekReorderCallbacks{
            [&]() { ++drained; },
            []() { return size_t(4); },
            {},
            [&]() { ++published; },
            []() { return std::optional<int64_t>{1'000'000}; },
            {},
        });
    if (!eof_result.drained_codec || !eof_result.published ||
        drained != 1 || published != 1) {
        return fail("exact seek reorder did not drain and publish at EOF");
    }

    drained = 0;
    published = 0;
    const auto preview_result = vr::handle_exact_seek_reorder_after_receive(
        vr::DecodeExactSeekReorderState{
            true,
            3,
            false,
            8,
            false,
            true,
        },
        vr::DecodeExactSeekReorderCallbacks{
            [&]() { ++drained; },
            []() { return size_t(3); },
            {},
            [&]() { ++published; },
            {},
            {},
        });
    if (preview_result.drained_codec || !preview_result.published ||
        drained != 0 || published != 1) {
        return fail("exact seek reorder did not publish preview-ready window");
    }

    return 0;
}

#include <catch2/catch_test_macros.hpp>

#include "video_renderer/decode/codec_loop.h"

extern "C" {
#include <libavutil/error.h>
}

using namespace vr;

TEST_CASE("CodecLoop: codec return values are classified explicitly",
          "[decode_thread][codec_loop]") {
    REQUIRE(classify_codec_loop_result(0) == CodecLoopResult::Ok);
    REQUIRE(classify_codec_loop_result(1) == CodecLoopResult::Ok);

    REQUIRE(classify_codec_loop_result(AVERROR(EAGAIN)) == CodecLoopResult::Again);
    REQUIRE(codec_loop_is_again_or_eof(AVERROR(EAGAIN)));

    REQUIRE(classify_codec_loop_result(AVERROR_EOF) == CodecLoopResult::EndOfStream);
    REQUIRE(codec_loop_is_again_or_eof(AVERROR_EOF));

    REQUIRE(classify_codec_loop_result(codec_loop_seh_caught_code()) ==
            CodecLoopResult::SehCaught);
    REQUIRE(codec_loop_is_seh_caught(codec_loop_seh_caught_code()));

    REQUIRE(classify_codec_loop_result(AVERROR_INVALIDDATA) == CodecLoopResult::Error);
    REQUIRE_FALSE(codec_loop_is_again_or_eof(AVERROR_INVALIDDATA));
}

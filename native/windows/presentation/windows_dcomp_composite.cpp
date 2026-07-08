#include "windows/presentation/windows_dcomp_composite.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr char kCompositeShader[] = R"(
Texture2D<float4> video_texture : register(t0);
Texture2D<float4> sdr_video_texture : register(t1);
SamplerState linear_sampler : register(s0);
cbuffer CompositeConstants : register(b0) {
  float4 viewport;
  float4 background_color;
  float sdr_white_scale;
  float output_mode;
  float sdr_video_is_scrgb;
  float present_padding;
};
struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
  float2 positions[4] = {
    float2(-1.0, -1.0), float2(-1.0, 1.0),
    float2(1.0, -1.0), float2(1.0, 1.0)
  };
  float2 uvs[4] = {
    float2(0.0, 1.0), float2(0.0, 0.0),
    float2(1.0, 1.0), float2(1.0, 0.0)
  };
  VSOut output;
  output.position = float4(positions[id], 0.0, 1.0);
  output.uv = uvs[id];
  return output;
}
float3 srgb_to_linear(float3 value) {
  float3 low = value / 12.92;
  float3 high = pow((value + 0.055) / 1.055, 2.4);
  return lerp(high, low, step(value, 0.04045));
}
float3 linear_to_srgb(float3 value) {
  value = max(value, 0.0);
  float3 low = value * 12.92;
  float3 high = 1.055 * pow(value, 1.0 / 2.4) - 0.055;
  return lerp(low, high, step(0.0031308, value));
}
float3 scrgb_to_sdr(float3 value, int transfer) {
  float3 scrgb = max(value, 0.0);
  if (transfer == 2 || transfer == 3) {
    scrgb *= 80.0 / 203.0;
    scrgb = scrgb / (1.0 + scrgb);
  } else {
    scrgb /= max(sdr_white_scale, 0.0001);
  }
  return saturate(linear_to_srgb(scrgb));
}
float4 output_background() {
  if (output_mode < 0.5) {
    return saturate(background_color);
  }
  return float4(
      srgb_to_linear(saturate(background_color.rgb)) * sdr_white_scale,
      background_color.a);
}
float4 PSVideo(VSOut input) : SV_TARGET {
  float4 video = output_background();
  if (input.uv.x >= viewport.x && input.uv.y >= viewport.y &&
      input.uv.x <= viewport.z && input.uv.y <= viewport.w) {
    float2 extent = max(viewport.zw - viewport.xy, float2(0.00001, 0.00001));
    float2 video_uv = (input.uv - viewport.xy) / extent;
    if (output_mode < 0.5) {
      if (sdr_video_is_scrgb > 0.5) {
        video = video_texture.Sample(linear_sampler, video_uv);
        video.rgb = scrgb_to_sdr(video.rgb, 0);
      } else {
        video = sdr_video_texture.Sample(linear_sampler, video_uv);
      }
    } else {
      video = video_texture.Sample(linear_sampler, video_uv);
    }
  }
  return video;
}
)";

float srgb_to_linear(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f
        ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

} // namespace

namespace vr {

const char* windows_dcomp_composite_hlsl() {
    return kCompositeShader;
}

WindowsDcompCompositeSample composite_windows_dcomp_pixel(
    const WindowsDcompCompositeSample& video,
    const WindowsDcompCompositeSample& flutter,
    float sdr_white_scale) {
    const float alpha = std::clamp(flutter.a, 0.0f, 1.0f);
    const float inverse_alpha = 1.0f - alpha;
    const auto flutter_channel = [&](float premultiplied_srgb) {
        if (alpha <= 0.00001f) {
            return 0.0f;
        }
        return srgb_to_linear(premultiplied_srgb / alpha) *
               alpha * sdr_white_scale;
    };
    return {
        flutter_channel(flutter.r) + video.r * inverse_alpha,
        flutter_channel(flutter.g) + video.g * inverse_alpha,
        flutter_channel(flutter.b) + video.b * inverse_alpha,
        alpha + video.a * inverse_alpha,
    };
}

float half_to_float(unsigned short half) {
    const double sign = (half & 0x8000u) != 0 ? -1.0 : 1.0;
    const int exponent = static_cast<int>((half >> 10u) & 0x1fu);
    const int mantissa = static_cast<int>(half & 0x03ffu);
    if (exponent == 0) {
        return mantissa == 0
            ? static_cast<float>(sign * 0.0)
            : static_cast<float>(
                  sign * std::ldexp(static_cast<double>(mantissa), -24));
    }
    if (exponent == 0x1f) {
        return mantissa == 0
            ? static_cast<float>(
                  sign * std::numeric_limits<double>::infinity())
            : std::numeric_limits<float>::quiet_NaN();
    }
    const double value = 1.0 + static_cast<double>(mantissa) / 1024.0;
    return static_cast<float>(sign * std::ldexp(value, exponent - 15));
}

} // namespace vr

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

int display_count_for_mode(int mode, int count) {
    return mode == 1 && count > 1 ? 2 : count;
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

WindowsSourceProjectionSample project_windows_source_sample(
    float video_u,
    float video_v,
    const WindowsSourceProjection& projection,
    const std::array<bool, 4>& source_present) {
    WindowsSourceProjectionSample result;
    if (!projection.enabled) {
        return result;
    }
    const int count = std::clamp(projection.active_track_count, 1, 4);
    const int display_count = display_count_for_mode(projection.mode, count);
    int display_slot = 0;
    float local_u = video_u;
    if (projection.mode == 0 && display_count > 1) {
        const float scaled =
            std::clamp(video_u, 0.0f, 0.999999f) * display_count;
        display_slot = std::clamp(
            static_cast<int>(std::floor(scaled)), 0, display_count - 1);
        local_u = scaled - display_slot;
    } else if (projection.mode == 1 && display_count > 1) {
        display_slot =
            video_u < std::clamp(projection.split_pos, 0.0001f, 0.9999f)
                ? 0
                : 1;
    }
    const int source_slot = std::clamp(
        projection.source_order[static_cast<size_t>(display_slot)], 0, 3);
    if (!source_present[static_cast<size_t>(source_slot)]) {
        return result;
    }
    const float u =
        (local_u - projection.display_offset_x[source_slot]) *
            projection.inv_display_size_x[source_slot] -
        projection.view_offset_uv_x[source_slot];
    const float v =
        (video_v - projection.display_offset_y[source_slot]) *
            projection.inv_display_size_y[source_slot] -
        projection.view_offset_uv_y[source_slot];
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return result;
    }
    result.present = true;
    result.source_slot = source_slot;
    result.u = u;
    result.v = v;
    return result;
}

std::array<WindowsRetainedSourceVisualRect, 4>
project_windows_retained_source_visuals(
    float viewport_left,
    float viewport_top,
    float viewport_right,
    float viewport_bottom,
    const WindowsSourceProjection& projection,
    const std::array<bool, 4>& source_present) {
    std::array<WindowsRetainedSourceVisualRect, 4> result{};
    if (!projection.enabled) {
        return result;
    }
    const float viewport_width =
        std::max(0.0f, viewport_right - viewport_left);
    const float viewport_height =
        std::max(0.0f, viewport_bottom - viewport_top);
    if (viewport_width <= 0.0f || viewport_height <= 0.0f) {
        return result;
    }
    const int count = std::clamp(projection.active_track_count, 1, 4);
    const int display_count = display_count_for_mode(projection.mode, count);
    for (int display_slot = 0; display_slot < display_count; ++display_slot) {
        const int source_slot = std::clamp(
            projection.source_order[static_cast<size_t>(display_slot)], 0, 3);
        if (!source_present[static_cast<size_t>(source_slot)]) {
            continue;
        }
        const float inv_x = projection.inv_display_size_x[source_slot];
        const float inv_y = projection.inv_display_size_y[source_slot];
        if (std::fabs(inv_x) < 0.00001f ||
            std::fabs(inv_y) < 0.00001f) {
            continue;
        }
        float projected_left = viewport_left;
        float projected_right = viewport_right;
        float clip_left = viewport_left;
        float clip_right = viewport_right;
        if (projection.mode == 0 && display_count > 1) {
            const float slot_width = viewport_width / display_count;
            projected_left = viewport_left + slot_width * display_slot;
            projected_right = projected_left + slot_width;
            clip_left = projected_left;
            clip_right = projected_right;
        } else if (projection.mode == 1 && display_count > 1) {
            const float split = std::clamp(
                projection.split_pos, 0.0001f, 0.9999f);
            const float split_x = viewport_left + viewport_width * split;
            if (display_slot == 0) {
                clip_left = viewport_left;
                clip_right = split_x;
            } else {
                clip_left = split_x;
                clip_right = viewport_right;
            }
        }
        const float projected_width =
            std::max(0.0f, projected_right - projected_left);
        if (projected_width <= 0.0f) {
            continue;
        }
        const float display_size_x = 1.0f / inv_x;
        const float display_size_y = 1.0f / inv_y;
        const float local_left =
            projection.display_offset_x[source_slot] +
            projection.view_offset_uv_x[source_slot] / inv_x;
        const float local_top =
            projection.display_offset_y[source_slot] +
            projection.view_offset_uv_y[source_slot] / inv_y;
        auto& rect = result[static_cast<size_t>(source_slot)];
        rect.present = true;
        rect.source_slot = source_slot;
        rect.left = projected_left + local_left * projected_width;
        rect.top = viewport_top + local_top * viewport_height;
        rect.right = rect.left + display_size_x * projected_width;
        rect.bottom = rect.top + display_size_y * viewport_height;
        rect.clip_left = clip_left;
        rect.clip_top = viewport_top;
        rect.clip_right = clip_right;
        rect.clip_bottom = viewport_bottom;
    }
    return result;
}

} // namespace vr

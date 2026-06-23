#include "windows/presentation/windows_dcomp_composite.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr char kCompositeShader[] = R"(
Texture2D<float4> video_texture : register(t0);
Texture2D<float4> flutter_texture : register(t1);
Texture2D<float4> source_texture_0 : register(t2);
Texture2D<float4> source_texture_1 : register(t3);
Texture2D<float4> source_texture_2 : register(t4);
Texture2D<float4> source_texture_3 : register(t5);
Texture2D<float4> sdr_video_texture : register(t6);
SamplerState linear_sampler : register(s0);
cbuffer CompositeConstants : register(b0) {
  float4 viewport;
  float sdr_white_scale;
  float output_mode;
  float source_projection_enabled;
  float source_mode;
  float source_split_pos;
  float source_track_count;
  float source_header_padding;
  float4 source_present;
  float4 source_order;
  float4 source_transfer;
  float4 source_display_offset_x;
  float4 source_display_offset_y;
  float4 source_inv_display_size_x;
  float4 source_inv_display_size_y;
  float4 source_view_offset_uv_x;
  float4 source_view_offset_uv_y;
  float4 background_color;
};
struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
struct OverlayIn {
  float2 source_uv : TEXCOORD0;
  float source_slot : TEXCOORD1;
  float4 color : COLOR;
};
struct OverlayOut {
  float4 position : SV_POSITION;
  float2 global_uv : TEXCOORD0;
  float4 clip_uv : TEXCOORD1;
  float4 color : COLOR;
};
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
float value_at(float4 values, int index);
int display_count_for_mode(int mode, int count);
int display_slot_for_source(int source_slot, int count);
OverlayOut VSOverlay(OverlayIn input) {
  OverlayOut output;
  int mode = (int)round(source_mode);
  int count = clamp((int)round(source_track_count), 1, 4);
  int display_count = display_count_for_mode(mode, count);
  int source_slot = clamp((int)round(input.source_slot), 0, 3);
  int display_slot = display_slot_for_source(source_slot, display_count);
  if (display_slot < 0 || value_at(source_present, source_slot) < 0.5) {
    output.position = float4(-4.0, -4.0, 0.0, 1.0);
    output.global_uv = 0.0;
    output.clip_uv = 0.0;
    output.color = 0.0;
    return output;
  }
  float2 display_offset = float2(
      value_at(source_display_offset_x, source_slot),
      value_at(source_display_offset_y, source_slot));
  float2 inv_display_size = float2(
      value_at(source_inv_display_size_x, source_slot),
      value_at(source_inv_display_size_y, source_slot));
  float2 view_offset = float2(
      value_at(source_view_offset_uv_x, source_slot),
      value_at(source_view_offset_uv_y, source_slot));
  if (abs(inv_display_size.x) < 0.00001 ||
      abs(inv_display_size.y) < 0.00001) {
    output.position = float4(-4.0, -4.0, 0.0, 1.0);
    output.global_uv = 0.0;
    output.clip_uv = 0.0;
    output.color = 0.0;
    return output;
  }
  float2 local_uv = display_offset +
      (input.source_uv + view_offset) / inv_display_size;
  float2 extent = max(viewport.zw - viewport.xy, float2(0.00001, 0.00001));
  float2 clip_min = viewport.xy;
  float2 clip_max = viewport.zw;
  if (mode == 0 && display_count > 1) {
    float slot_left = (float)display_slot / (float)display_count;
    float slot_right = (float)(display_slot + 1) / (float)display_count;
    local_uv.x = slot_left + local_uv.x / (float)display_count;
    clip_min.x = viewport.x + slot_left * extent.x;
    clip_max.x = viewport.x + slot_right * extent.x;
  } else if (mode == 1 && display_count > 1) {
    float split = clamp(source_split_pos, 0.0001, 0.9999);
    float split_x = viewport.x + split * extent.x;
    if (display_slot == 0) {
      clip_max.x = split_x;
    } else {
      clip_min.x = split_x;
    }
  }
  float2 global_uv = viewport.xy + local_uv * extent;
  output.position = float4(
      global_uv.x * 2.0 - 1.0,
      1.0 - global_uv.y * 2.0,
      0.0,
      1.0);
  output.global_uv = global_uv;
  output.clip_uv = float4(clip_min, clip_max);
  output.color = input.color;
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
float value_at(float4 values, int index) {
  if (index == 0) return values.x;
  if (index == 1) return values.y;
  if (index == 2) return values.z;
  return values.w;
}
int display_slot_for_source(int source_slot, int count) {
  [unroll]
  for (int i = 0; i < 4; ++i) {
    if (i < count && (int)round(value_at(source_order, i)) == source_slot) {
      return i;
    }
  }
  return -1;
}
int display_count_for_mode(int mode, int count) {
  return mode == 1 && count > 1 ? 2 : count;
}
float4 sample_source(int slot, float2 uv) {
  if (slot == 0) return source_texture_0.Sample(linear_sampler, uv);
  if (slot == 1) return source_texture_1.Sample(linear_sampler, uv);
  if (slot == 2) return source_texture_2.Sample(linear_sampler, uv);
  return source_texture_3.Sample(linear_sampler, uv);
}
float4 output_background() {
  if (output_mode < 0.5) {
    return saturate(background_color);
  }
  return float4(
      srgb_to_linear(saturate(background_color.rgb)) * sdr_white_scale,
      background_color.a);
}
float4 source_projected_video(float2 video_uv) {
  int mode = (int)round(source_mode);
  int count = clamp((int)round(source_track_count), 1, 4);
  int display_count = display_count_for_mode(mode, count);
  int display_slot = 0;
  float2 local_uv = video_uv;
  if (mode == 0 && display_count > 1) {
    float scaled_x = clamp(video_uv.x, 0.0, 0.999999) * display_count;
    display_slot = clamp((int)floor(scaled_x), 0, display_count - 1);
    local_uv.x = scaled_x - display_slot;
  } else if (mode == 1 && display_count > 1) {
    display_slot = video_uv.x < clamp(source_split_pos, 0.0001, 0.9999)
        ? 0 : 1;
  }
  int source_slot =
      clamp((int)round(value_at(source_order, display_slot)), 0, 3);
  if (value_at(source_present, source_slot) < 0.5) {
    return output_background();
  }
  float2 display_offset = float2(
      value_at(source_display_offset_x, source_slot),
      value_at(source_display_offset_y, source_slot));
  float2 inv_display_size = float2(
      value_at(source_inv_display_size_x, source_slot),
      value_at(source_inv_display_size_y, source_slot));
  float2 view_offset = float2(
      value_at(source_view_offset_uv_x, source_slot),
      value_at(source_view_offset_uv_y, source_slot));
  float2 source_uv =
      (local_uv - display_offset) * inv_display_size - view_offset;
  if (source_uv.x < 0.0 || source_uv.x > 1.0 ||
      source_uv.y < 0.0 || source_uv.y > 1.0) {
    return output_background();
  }
  float4 source = sample_source(source_slot, source_uv);
  if (output_mode < 0.5) {
    source.rgb = scrgb_to_sdr(
        source.rgb, (int)round(value_at(source_transfer, source_slot)));
  }
  return source;
}
float4 PSVideo(VSOut input) : SV_TARGET {
  float4 video = output_background();
  if (input.uv.x >= viewport.x && input.uv.y >= viewport.y &&
      input.uv.x <= viewport.z && input.uv.y <= viewport.w) {
    float2 extent = max(viewport.zw - viewport.xy, float2(0.00001, 0.00001));
    float2 video_uv = (input.uv - viewport.xy) / extent;
    if (source_projection_enabled > 0.5) {
      video = source_projected_video(video_uv);
    } else if (output_mode < 0.5) {
      video = sdr_video_texture.Sample(linear_sampler, video_uv);
    } else {
      video = video_texture.Sample(linear_sampler, video_uv);
    }
  }
  return video;
}
float4 PSFlutter(VSOut input) : SV_TARGET {
  float4 flutter = flutter_texture.Sample(linear_sampler, input.uv);
  if (output_mode < 0.5) {
    return flutter;
  }
  float alpha = saturate(flutter.a);
  float3 straight_srgb = alpha > 0.00001 ? flutter.rgb / alpha : 0.0;
  float3 flutter_premul_linear =
      srgb_to_linear(saturate(straight_srgb)) * alpha * sdr_white_scale;
  return float4(flutter_premul_linear, alpha);
}
float4 PSMain(VSOut input) : SV_TARGET {
  float4 video = PSVideo(input);
  float4 flutter = PSFlutter(input);
  return float4(
      flutter.rgb + video.rgb * (1.0 - flutter.a),
      flutter.a + video.a * (1.0 - flutter.a));
}
float4 PSOverlay(OverlayOut input) : SV_TARGET {
  if (input.global_uv.x < input.clip_uv.x ||
      input.global_uv.y < input.clip_uv.y ||
      input.global_uv.x > input.clip_uv.z ||
      input.global_uv.y > input.clip_uv.w) {
    discard;
  }
  return input.color;
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

#include "renderer/overlay/analysis_overlay_gpu_geometry.h"

#include "renderer/layout/layout_geometry.h"

#include <algorithm>
#include <cmath>

namespace vr {
namespace {

struct FloatRect { float left, top, right, bottom; };

int display_count(const ShaderConstants& c) {
  return std::clamp(c.track_count, 1, 4);
}

int display_slot(const ShaderConstants& c, int track_slot) {
  for (int i = 0; i < display_count(c); ++i) {
    if (c.order[i] == track_slot) return i;
  }
  return -1;
}

bool clip_for_track(const ShaderConstants& c, int slot, int width, int height,
                    FloatRect& out) {
  out = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
  if (c.mode == LAYOUT_SPLIT_SCREEN) {
    const float split = std::clamp(c.split_pos, 0.0f, 1.0f) * width;
    if (c.order[0] == slot) { out.right = split; return true; }
    if (c.order[1] == slot) { out.left = split; return true; }
    return false;
  }
  const int display = display_slot(c, slot);
  if (display < 0) return false;
  const float cell = static_cast<float>(width) / display_count(c);
  out.left = display * cell;
  out.right = (display + 1) * cell;
  return true;
}

bool project(const ShaderConstants& c, int slot, int width, int height,
             float u, float v, float& x, float& y) {
  if (slot < 0 || slot >= 4 || c.inv_display_size_x[slot] == 0.0f ||
      c.inv_display_size_y[slot] == 0.0f) return false;
  const float local_x = c.display_offset_x[slot] +
      (u + c.view_offset_uv_x[slot]) / c.inv_display_size_x[slot];
  const float local_y = c.display_offset_y[slot] +
      (v + c.view_offset_uv_y[slot]) / c.inv_display_size_y[slot];
  if (!std::isfinite(local_x) || !std::isfinite(local_y)) return false;
  if (c.mode == LAYOUT_SPLIT_SCREEN) {
    x = local_x * width;
  } else {
    const int display = display_slot(c, slot);
    if (display < 0) return false;
    x = (display + local_x) * width / display_count(c);
  }
  y = local_y * height;
  return true;
}

AnalysisOverlayGpuVertex make_vertex(float x, float y, int width, int height,
                                     analysis::OverlayColor color) {
  constexpr float scale = 1.0f / 255.0f;
  return {x * 2.0f / width - 1.0f, 1.0f - y * 2.0f / height,
          color.r * scale, color.g * scale, color.b * scale, color.a * scale};
}

bool add_rect(AnalysisOverlayGpuGeometry& out, FloatRect rect,
              const FloatRect& clip, int width, int height,
              analysis::OverlayColor color, bool line) {
  rect.left = std::max(rect.left, clip.left);
  rect.top = std::max(rect.top, clip.top);
  rect.right = std::min(rect.right, clip.right);
  rect.bottom = std::min(rect.bottom, clip.bottom);
  if (rect.left >= rect.right || rect.top >= rect.bottom || color.a == 0) return false;
  const auto a = make_vertex(rect.left, rect.top, width, height, color);
  const auto b = make_vertex(rect.right, rect.top, width, height, color);
  const auto c = make_vertex(rect.left, rect.bottom, width, height, color);
  const auto d = make_vertex(rect.right, rect.bottom, width, height, color);
  out.vertices.insert(out.vertices.end(), {a, b, c, c, b, d});
  line ? ++out.line_rect_count : ++out.fill_rect_count;
  return true;
}

bool clip_line(float& x0, float& y0, float& x1, float& y1,
               const FloatRect& clip) {
  const float dx = x1 - x0, dy = y1 - y0;
  float t0 = 0.0f, t1 = 1.0f;
  auto edge = [&](float p, float q) {
    if (p == 0.0f) return q >= 0.0f;
    const float r = q / p;
    if (p < 0.0f) { if (r > t1) return false; t0 = std::max(t0, r); }
    else { if (r < t0) return false; t1 = std::min(t1, r); }
    return true;
  };
  if (!edge(-dx, x0 - clip.left) || !edge(dx, clip.right - x0) ||
      !edge(-dy, y0 - clip.top) || !edge(dy, clip.bottom - y0)) return false;
  const float ox = x0, oy = y0;
  x0 = ox + t0 * dx; y0 = oy + t0 * dy;
  x1 = ox + t1 * dx; y1 = oy + t1 * dy;
  return true;
}

void add_line(AnalysisOverlayGpuGeometry& out, float x0, float y0,
              float x1, float y1, const FloatRect& clip, int width, int height,
              analysis::OverlayColor color) {
  if (!clip_line(x0, y0, x1, y1, clip) || color.a == 0) return;
  const float dx = x1 - x0, dy = y1 - y0;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (length < 0.001f) return;
  const float nx = -dy / length, ny = dx / length;
  const auto a = make_vertex(x0 + nx, y0 + ny, width, height, color);
  const auto b = make_vertex(x1 + nx, y1 + ny, width, height, color);
  const auto c = make_vertex(x0 - nx, y0 - ny, width, height, color);
  const auto d = make_vertex(x1 - nx, y1 - ny, width, height, color);
  out.vertices.insert(out.vertices.end(), {a, b, c, c, b, d});
  ++out.line_rect_count;
}

}  // namespace

AnalysisOverlayGpuGeometry build_analysis_overlay_gpu_geometry(
    const AnalysisOverlayPrimitivePackage& package, const ShaderConstants& constants,
    int target_width, int target_height) {
  AnalysisOverlayGpuGeometry out;
  if (target_width <= 0 || target_height <= 0) return out;
  size_t count = 0;
  for (const auto& track : package.tracks) count += track.fill_rects.size() +
      track.outline_rects.size() * 4 + track.motion_lines.size();
  out.vertices.reserve(count * 6);
  for (const auto& track : package.tracks) {
    if (track.slot < 0 || track.video_width <= 0 || track.video_height <= 0) continue;
    FloatRect clip{};
    if (!clip_for_track(constants, track.slot, target_width, target_height, clip)) continue;
    auto point = [&](int x, int y, float& tx, float& ty) {
      return project(constants, track.slot, target_width, target_height,
                     static_cast<float>(x) / track.video_width,
                     static_cast<float>(y) / track.video_height, tx, ty);
    };
    for (const auto& rect : track.fill_rects) {
      float x0, y0, x1, y1;
      if (point(rect.x0, rect.y0, x0, y0) && point(rect.x1, rect.y1, x1, y1))
        add_rect(out, {std::min(x0, x1), std::min(y0, y1),
                       std::max(x0, x1), std::max(y0, y1)},
                 clip, target_width, target_height, rect.color, false);
    }
    for (const auto& rect : track.outline_rects) {
      float x0, y0, x1, y1;
      if (!point(rect.x0, rect.y0, x0, y0) || !point(rect.x1, rect.y1, x1, y1)) continue;
      const float l = std::min(x0, x1), t = std::min(y0, y1);
      const float r = std::max(x0, x1), b = std::max(y0, y1), w = 2.0f;
      add_rect(out, {l, t, r, t + w}, clip, target_width, target_height, rect.color, true);
      add_rect(out, {l, t, l + w, b}, clip, target_width, target_height, rect.color, true);
      add_rect(out, {r - w, t, r, b}, clip, target_width, target_height, rect.color, true);
      add_rect(out, {l, b - w, r, b}, clip, target_width, target_height, rect.color, true);
    }
    for (const auto& line : track.motion_lines) {
      float x0, y0, x1, y1;
      if (point(line.x0, line.y0, x0, y0) && point(line.x1, line.y1, x1, y1))
        add_line(out, x0, y0, x1, y1, clip, target_width, target_height, line.color);
    }
  }
  return out;
}

}  // namespace vr

#include "renderer/overlay/analysis_overlay_gpu_geometry.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace vr {
namespace {

struct CachedGpuPrimitiveBatch {
  uint64_t generation = 0;
  std::weak_ptr<const AnalysisOverlayGpuPrimitiveBatch> batch;
  uint64_t last_used = 0;
};

constexpr size_t kGpuPrimitiveCacheLimit = 24;

std::mutex& cache_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<CachedGpuPrimitiveBatch>& cache_entries() {
  static std::vector<CachedGpuPrimitiveBatch> entries;
  return entries;
}

uint64_t& cache_clock() {
  static uint64_t clock = 0;
  return clock;
}

AnalysisOverlayGpuPrimitive make_primitive(
    const AnalysisOverlayTrackPrimitives& track,
    float x0,
    float y0,
    float x1,
    float y1,
    analysis::OverlayColor color,
    AnalysisOverlayGpuPrimitiveKind kind) {
  constexpr float kColorScale = 1.0f / 255.0f;
  AnalysisOverlayGpuPrimitive primitive;
  primitive.source_uv0[0] = x0 / static_cast<float>(track.video_width);
  primitive.source_uv0[1] = y0 / static_cast<float>(track.video_height);
  primitive.source_uv1[0] = x1 / static_cast<float>(track.video_width);
  primitive.source_uv1[1] = y1 / static_cast<float>(track.video_height);
  primitive.color[0] = color.r * kColorScale;
  primitive.color[1] = color.g * kColorScale;
  primitive.color[2] = color.b * kColorScale;
  primitive.color[3] = color.a * kColorScale;
  primitive.track_slot = static_cast<uint32_t>(track.slot);
  primitive.kind = static_cast<uint32_t>(kind);
  return primitive;
}

std::shared_ptr<const AnalysisOverlayGpuPrimitiveBatch> build_batch(
    const AnalysisOverlayPrimitivePackage& package) {
  auto batch = std::make_shared<AnalysisOverlayGpuPrimitiveBatch>();
  batch->source_generation = package.cache_generation;

  size_t fill_count = 0;
  size_t contrast_capacity = 0;
  size_t motion_count = 0;
  for (const auto& track : package.tracks) {
    fill_count += track.fill_rects.size();
    contrast_capacity += track.outline_rects.size() * 4;
    motion_count += track.motion_lines.size();
  }
  std::vector<AnalysisOverlayGpuPrimitive> fills;
  std::vector<AnalysisOverlayGpuPrimitive> contrasts;
  std::vector<AnalysisOverlayGpuPrimitive> motions;
  fills.reserve(fill_count);
  contrasts.reserve(contrast_capacity);
  motions.reserve(motion_count);

  for (const auto& track : package.tracks) {
    if (track.slot < 0 || track.slot >= 4 || track.video_width <= 0 ||
        track.video_height <= 0) {
      continue;
    }
    for (const auto& rect : track.fill_rects) {
      if (rect.color.a == 0) {
        continue;
      }
      fills.push_back(make_primitive(
          track, static_cast<float>(rect.x0), static_cast<float>(rect.y0),
          static_cast<float>(rect.x1), static_cast<float>(rect.y1), rect.color,
          AnalysisOverlayGpuPrimitiveKind::FillRect));
    }
    for (const auto& rect : track.outline_rects) {
      if (rect.color.a == 0) {
        continue;
      }
      const float left = static_cast<float>(std::min(rect.x0, rect.x1));
      const float top = static_cast<float>(std::min(rect.y0, rect.y1));
      const float right = static_cast<float>(std::max(rect.x0, rect.x1));
      const float bottom = static_cast<float>(std::max(rect.y0, rect.y1));
      contrasts.push_back(make_primitive(
          track, left, top, left, bottom, rect.color,
          AnalysisOverlayGpuPrimitiveKind::ContrastVertical));
      contrasts.push_back(make_primitive(
          track, left, top, right, top, rect.color,
          AnalysisOverlayGpuPrimitiveKind::ContrastHorizontal));
      if (std::max(rect.x0, rect.x1) >= track.video_width) {
        contrasts.push_back(make_primitive(
            track, right, top, right, bottom, rect.color,
            AnalysisOverlayGpuPrimitiveKind::ContrastVertical));
      }
      if (std::max(rect.y0, rect.y1) >= track.video_height) {
        contrasts.push_back(make_primitive(
            track, left, bottom, right, bottom, rect.color,
            AnalysisOverlayGpuPrimitiveKind::ContrastHorizontal));
      }
    }
    for (const auto& line : track.motion_lines) {
      if (line.color.a == 0 || (line.x0 == line.x1 && line.y0 == line.y1)) {
        continue;
      }
      motions.push_back(make_primitive(
          track, static_cast<float>(line.x0), static_cast<float>(line.y0),
          static_cast<float>(line.x1), static_cast<float>(line.y1), line.color,
          AnalysisOverlayGpuPrimitiveKind::MotionLine));
    }
  }

  batch->fill_count = fills.size();
  batch->contrast_count = contrasts.size();
  batch->motion_count = motions.size();
  batch->primitives.reserve(
      batch->fill_count + batch->contrast_count + batch->motion_count);
  batch->primitives.insert(batch->primitives.end(), fills.begin(), fills.end());
  batch->primitives.insert(
      batch->primitives.end(), contrasts.begin(), contrasts.end());
  batch->primitives.insert(
      batch->primitives.end(), motions.begin(), motions.end());
  return batch;
}

}  // namespace

AnalysisOverlayGpuPrimitiveLookup lookup_analysis_overlay_gpu_primitives(
    const AnalysisOverlayPrimitivePackage& package) {
  if (package.cache_generation != 0) {
    std::lock_guard<std::mutex> lock(cache_mutex());
    auto& clock = cache_clock();
    const uint64_t use_token = ++clock;
    auto& entries = cache_entries();
    for (auto entry = entries.begin(); entry != entries.end();) {
      if (entry->generation == package.cache_generation) {
        if (auto batch = entry->batch.lock()) {
          entry->last_used = use_token;
          return {std::move(batch), true};
        }
        entry = entries.erase(entry);
        continue;
      }
      ++entry;
    }
  }

  auto batch = build_batch(package);
  if (package.cache_generation == 0 || !batch) {
    return {std::move(batch), false};
  }

  std::lock_guard<std::mutex> lock(cache_mutex());
  auto& clock = cache_clock();
  auto& entries = cache_entries();
  const uint64_t use_token = ++clock;
  for (auto entry = entries.begin(); entry != entries.end();) {
    if (entry->generation == package.cache_generation) {
      if (auto cached = entry->batch.lock()) {
        entry->last_used = use_token;
        return {std::move(cached), true};
      }
      entry = entries.erase(entry);
      continue;
    }
    ++entry;
  }
  if (entries.size() >= kGpuPrimitiveCacheLimit) {
    const auto oldest = std::min_element(
        entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
          return lhs.last_used < rhs.last_used;
        });
    if (oldest != entries.end()) {
      entries.erase(oldest);
    }
  }
  entries.push_back(
      CachedGpuPrimitiveBatch{package.cache_generation, batch, use_token});
  return {std::move(batch), false};
}

}  // namespace vr

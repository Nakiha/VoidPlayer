#pragma once

#include "renderer/overlay/analysis_overlay_primitives.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace vr {

enum class AnalysisOverlayGpuPrimitiveKind : uint32_t {
  FillRect = 0,
  ContrastVertical = 1,
  ContrastHorizontal = 2,
  MotionLine = 3,
};

// Platform-neutral source-space GPU input. Layout projection deliberately stays
// in the platform shader so pan/zoom does not rebuild or re-upload CU geometry.
struct AnalysisOverlayGpuPrimitive {
  float source_uv0[2] = {};
  float source_uv1[2] = {};
  float color[4] = {};
  uint32_t track_slot = 0;
  uint32_t kind = 0;
};

static_assert(sizeof(AnalysisOverlayGpuPrimitive) == 40);
static_assert(offsetof(AnalysisOverlayGpuPrimitive, source_uv0) == 0);
static_assert(offsetof(AnalysisOverlayGpuPrimitive, source_uv1) == 8);
static_assert(offsetof(AnalysisOverlayGpuPrimitive, color) == 16);
static_assert(offsetof(AnalysisOverlayGpuPrimitive, track_slot) == 32);
static_assert(offsetof(AnalysisOverlayGpuPrimitive, kind) == 36);

struct AnalysisOverlayGpuPrimitiveBatch {
  uint64_t source_generation = 0;
  std::vector<AnalysisOverlayGpuPrimitive> primitives;
  size_t fill_count = 0;
  size_t contrast_count = 0;
  size_t motion_count = 0;

  bool empty() const { return primitives.empty(); }
  size_t line_rect_count() const { return contrast_count + motion_count; }
};

struct AnalysisOverlayGpuPrimitiveLookup {
  std::shared_ptr<const AnalysisOverlayGpuPrimitiveBatch> batch;
  bool cache_hit = false;
};

// Reuses source-space GPU primitives for a cached analysis package. Generation
// zero is intentionally uncached for tests and transient/incomplete packages.
AnalysisOverlayGpuPrimitiveLookup lookup_analysis_overlay_gpu_primitives(
    const AnalysisOverlayPrimitivePackage& package);

}  // namespace vr

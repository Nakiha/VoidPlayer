#pragma once

namespace vr {

struct WindowsCompositorViewportRect {
  int left = 0;
  int top = 0;
  int width = 0;
  int height = 0;
  int surface_width = 0;
  int surface_height = 0;
};

// A horizontal-only target resize is the sidebar/window-width case. Align its
// destination width with the newly rendered target during retained handoff so
// the new narrow/wide image is never sampled through the previous rectangle.
// Position and surface changes still wait for Flutter's authoritative rect.
inline bool synchronize_retained_horizontal_viewport_handoff(
    WindowsCompositorViewportRect& viewport, int target_width,
    int target_height) {
  if (viewport.width <= 0 || viewport.height <= 0 ||
      viewport.surface_width <= 0 || viewport.surface_height <= 0 ||
      target_width <= 0 || target_height <= 0 ||
      target_height != viewport.height || target_width == viewport.width) {
    return false;
  }
  if (viewport.left < 0 || viewport.left >= viewport.surface_width ||
      target_width > viewport.surface_width - viewport.left) {
    return false;
  }
  viewport.width = target_width;
  return true;
}

}  // namespace vr

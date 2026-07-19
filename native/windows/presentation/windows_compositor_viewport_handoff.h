#pragma once

#include <cstdint>

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

// Runner-owned D3D11 targets cross an asynchronous raw-pointer callback
// boundary. Keep a retired ring alive until several successful compositor GPU
// completions have passed after the first frame from its replacement. This
// covers the two interaction submissions that may already be in flight plus
// the compositor's retained/presented buffers without relying on wall time.
class WindowsRetiredTargetReleaseGate {
 public:
  static constexpr uint32_t kRequiredSuccessfulComposites = 4;

  void reset() {
    first_replacement_serial_ = 0;
    successful_composites_remaining_ = 0;
  }

  void arm(uint64_t first_replacement_serial) {
    if (first_replacement_serial == 0 || armed()) {
      return;
    }
    first_replacement_serial_ = first_replacement_serial;
    successful_composites_remaining_ = kRequiredSuccessfulComposites;
  }

  bool note_completion(uint64_t serial, bool success) {
    if (!armed() || !success || serial < first_replacement_serial_) {
      return false;
    }
    if (successful_composites_remaining_ > 0) {
      --successful_composites_remaining_;
    }
    if (successful_composites_remaining_ != 0) {
      return false;
    }
    reset();
    return true;
  }

  bool armed() const { return first_replacement_serial_ != 0; }
  uint32_t successful_composites_remaining() const {
    return successful_composites_remaining_;
  }

 private:
  uint64_t first_replacement_serial_ = 0;
  uint32_t successful_composites_remaining_ = 0;
};

}  // namespace vr

#include "renderer/layout/layout_controller.h"

#include <algorithm>
#include <cstddef>

namespace vr {

void LayoutController::reset(LayoutState& layout) {
    layout = LayoutState();
    for (int i = 0; i < 4; ++i) {
        file_id_order_[i] = -1;
        layout.order[i] = 0;
    }
}

void LayoutController::apply(LayoutState& layout,
                             const LayoutState& requested,
                             const SlotResolver& resolve_slot) {
    layout.mode = requested.mode;
    layout.split_pos =
        std::clamp(requested.split_pos, kMinLayoutSplitPos, kMaxLayoutSplitPos);
    layout.zoom_ratio =
        std::clamp(requested.zoom_ratio, kMinLayoutZoomRatio, kMaxLayoutZoomRatio);
    layout.view_offset[0] = requested.view_offset[0];
    layout.view_offset[1] = requested.view_offset[1];
    layout.pixel_size_mode =
        (requested.pixel_size_mode == PIXEL_SIZE_FILL_VIEW)
            ? PIXEL_SIZE_FILL_VIEW
            : PIXEL_SIZE_UNIFORM_VIDEO_PIXELS;

    for (int i = 0; i < 4; ++i) {
        file_id_order_[i] = requested.order[i];
    }
    rebuild_slot_order(layout, resolve_slot);
}

LayoutState LayoutController::snapshot(LayoutState layout) const {
    for (int i = 0; i < 4; ++i) {
        layout.order[i] = file_id_order_[i];
    }
    return layout;
}

void LayoutController::append_track(LayoutState& layout, int file_id, int slot) {
    if (file_id < 0 || slot < 0) {
        return;
    }
    for (int i = 0; i < 4; ++i) {
        if (file_id_order_[i] < 0) {
            file_id_order_[i] = file_id;
            layout.order[i] = slot;
            return;
        }
    }
}

void LayoutController::append_tracks(LayoutState& layout,
                                     const TrackPipelineManager& tracks) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        append_track(layout, tracks[i]->file_id, static_cast<int>(i));
    }
}

void LayoutController::remove_track(LayoutState& layout,
                                    int file_id,
                                    const SlotResolver& resolve_slot) {
    for (int i = 0; i < 4; ++i) {
        if (file_id_order_[i] == file_id) {
            for (int j = i; j < 3; ++j) {
                file_id_order_[j] = file_id_order_[j + 1];
            }
            file_id_order_[3] = -1;
            break;
        }
    }
    rebuild_slot_order(layout, resolve_slot);
}

void LayoutController::rebuild_slot_order(LayoutState& layout,
                                          const SlotResolver& resolve_slot) const {
    for (int i = 0; i < 4; ++i) {
        const int slot = resolve_slot ? resolve_slot(file_id_order_[i]) : -1;
        layout.order[i] = (slot >= 0) ? slot : 0;
    }
}

} // namespace vr

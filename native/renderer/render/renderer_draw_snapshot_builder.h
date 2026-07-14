#pragma once

#include "renderer/layout/renderer_layout_state.h"
#include "renderer/render/renderer_draw_snapshot.h"
#include "renderer/render/renderer_surface_state.h"
#include "renderer/track/renderer_track_controller.h"

#include "common/logging.h"

#include <spdlog/spdlog.h>

namespace vr {

// Render-domain helper for translating current track/layout/surface state into
// the immutable draw snapshot consumed by presentation backends.
class RendererDrawSnapshotBuilder {
public:
    static void update_track_geometry_from_decision(
        RendererTrackController& tracks,
        const PresentDecision& decision) {
        const auto updates =
            tracks.update_layout_track_geometry_from_decision(decision);
        for (const auto& update : updates) {
            spdlog::info(
                "[Renderer] track[{}] display geometry changed: {}x{} aspect={:.6f} -> {}x{} aspect={:.6f}",
                update.slot,
                update.old_width,
                update.old_height,
                update.old_aspect,
                update.new_width,
                update.new_height,
                update.new_aspect);
        }
    }

    static RendererDrawSnapshot build(RendererTrackController& tracks,
                                      RendererLayoutState& layout,
                                      const RendererSurfaceState& surface,
                                      const PresentDecision& decision) {
        RendererDrawSnapshot snapshot;
        snapshot.decision = decision;
        tracks.filter_present_decision(snapshot.decision);
        snapshot.layout_revision = layout.current_revision();
        snapshot.layout = layout.current_for_draw();
        snapshot.track_geometry = tracks.layout_track_geometry();
        tracks.populate_draw_tracks(snapshot.tracks);
        layout.copy_background_color(snapshot.background_color);
        snapshot.target_width = surface.width();
        snapshot.target_height = surface.height();
        return snapshot;
    }
};

} // namespace vr

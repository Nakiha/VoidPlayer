import '../analysis/analysis_overlay.dart';

/// Release-supported UI automation commands that are not user/player actions.
sealed class AutomationAction {
  final String name;

  const AutomationAction(this.name);
}

class SetRenderSize extends AutomationAction {
  final int width;
  final int height;

  const SetRenderSize(this.width, this.height) : super('SET_RENDER_SIZE');
}

class CaptureViewportAction extends AutomationAction {
  final String nameId;
  final String? outputPath;

  const CaptureViewportAction(this.nameId, {this.outputPath})
    : super('CAPTURE_VIEWPORT');
}

class WindowMaximize extends AutomationAction {
  const WindowMaximize() : super('WINDOW_MAXIMIZE');
}

class WindowRestore extends AutomationAction {
  const WindowRestore() : super('WINDOW_RESTORE');
}

class StoreViewCenter extends AutomationAction {
  final String nameId;

  const StoreViewCenter(this.nameId) : super('STORE_VIEW_CENTER');
}

class StoreResourceUsage extends AutomationAction {
  final String nameId;

  const StoreResourceUsage(this.nameId) : super('STORE_RESOURCE_USAGE');
}

class StoreNativeSeekCount extends AutomationAction {
  final String nameId;

  const StoreNativeSeekCount(this.nameId) : super('STORE_NATIVE_SEEK_COUNT');
}

class ToggleAnalysisOverlay extends AutomationAction {
  final int slotIndex;

  const ToggleAnalysisOverlay(this.slotIndex)
    : super('TOGGLE_ANALYSIS_OVERLAY');
}

class ToggleAnalysisOverlayPanel extends AutomationAction {
  const ToggleAnalysisOverlayPanel() : super('TOGGLE_ANALYSIS_OVERLAY_PANEL');
}

class SetAnalysisOverlayType extends AutomationAction {
  final AnalysisOverlayType type;

  const SetAnalysisOverlayType(this.type) : super('SET_ANALYSIS_OVERLAY_TYPE');
}

class SetAnalysisOverlayLayers extends AutomationAction {
  final Set<AnalysisOverlayLayer> layers;

  const SetAnalysisOverlayLayers(this.layers)
    : super('SET_ANALYSIS_OVERLAY_LAYERS');
}

class SetAnalysisOverlayOpacity extends AutomationAction {
  final double opacity;

  const SetAnalysisOverlayOpacity(this.opacity)
    : super('SET_ANALYSIS_OVERLAY_OPACITY');
}

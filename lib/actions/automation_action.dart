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

class CaptureViewportRegionAction extends AutomationAction {
  final String nameId;
  final int x;
  final int y;
  final int width;
  final int height;
  final int maxSize;
  final String? outputPath;

  const CaptureViewportRegionAction(
    this.nameId, {
    required this.x,
    required this.y,
    required this.width,
    required this.height,
    required this.maxSize,
    this.outputPath,
  }) : super('CAPTURE_VIEWPORT_REGION');
}

class CaptureFlutterAction extends AutomationAction {
  final String nameId;
  final String? outputPath;

  const CaptureFlutterAction(this.nameId, {this.outputPath})
    : super('CAPTURE_FLUTTER');
}

class CaptureWindowAction extends AutomationAction {
  final String nameId;
  final String? outputPath;

  const CaptureWindowAction(this.nameId, {this.outputPath})
    : super('CAPTURE_WINDOW');
}

class DebugFlutterSurfaceInfoAction extends AutomationAction {
  const DebugFlutterSurfaceInfoAction() : super('DEBUG_FLUTTER_SURFACE_INFO');
}

class DebugNativeCompositorAction extends AutomationAction {
  const DebugNativeCompositorAction() : super('DEBUG_NATIVE_COMPOSITOR');
}

class DebugFailNativeCompositorAction extends AutomationAction {
  final String reason;

  const DebugFailNativeCompositorAction({
    this.reason = 'ui-test-forced-failure',
  }) : super('DEBUG_FAIL_NATIVE_COMPOSITOR');
}

class DebugSimulateWindowsDeviceLossAction extends AutomationAction {
  final String target;
  final String reason;

  const DebugSimulateWindowsDeviceLossAction({
    required this.target,
    this.reason = 'debug-simulated-device-loss',
  }) : super('DEBUG_SIMULATE_WINDOWS_DEVICE_LOSS');
}

class ResetNativePerfCountersAction extends AutomationAction {
  const ResetNativePerfCountersAction() : super('RESET_NATIVE_PERF_COUNTERS');
}

class BeginNativeInteractionSampleAction extends AutomationAction {
  final String label;

  const BeginNativeInteractionSampleAction(this.label)
    : super('BEGIN_NATIVE_INTERACTION_SAMPLE');
}

class EndNativeInteractionSampleAction extends AutomationAction {
  final String label;

  const EndNativeInteractionSampleAction(this.label)
    : super('END_NATIVE_INTERACTION_SAMPLE');
}

class DebugNativeTimingAction extends AutomationAction {
  final String label;

  const DebugNativeTimingAction({this.label = ''})
    : super('DEBUG_NATIVE_TIMING');
}

class DebugFlutterTimingAction extends AutomationAction {
  const DebugFlutterTimingAction() : super('DEBUG_FLUTTER_TIMING');
}

class AssertFlutterTimingAction extends AutomationAction {
  final int minFrames;
  final int maxTotalP95Ms;
  final int maxOver33Ms;
  final int maxOver16Ms;

  const AssertFlutterTimingAction({
    required this.minFrames,
    required this.maxTotalP95Ms,
    required this.maxOver33Ms,
    this.maxOver16Ms = 1000000,
  }) : super('ASSERT_FLUTTER_TIMING');
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

class ClickFlutterPoint extends AutomationAction {
  final double x;
  final double y;

  const ClickFlutterPoint(this.x, this.y) : super('CLICK_FLUTTER_POINT');
}

class ClickControlsPlayButton extends AutomationAction {
  const ClickControlsPlayButton() : super('CLICK_CONTROLS_PLAY_BUTTON');
}

class ClickToolbarMediaInfoNative extends AutomationAction {
  const ClickToolbarMediaInfoNative()
    : super('CLICK_TOOLBAR_MEDIA_INFO_NATIVE');
}

class DragViewport extends AutomationAction {
  final double dx;
  final double dy;
  final int steps;
  final int stepMs;

  const DragViewport(this.dx, this.dy, {this.steps = 24, this.stepMs = 16})
    : super('DRAG_VIEWPORT');
}

class DragViewportNative extends AutomationAction {
  final double dx;
  final double dy;
  final int steps;
  final int stepMs;
  final String button;

  const DragViewportNative(
    this.dx,
    this.dy, {
    this.steps = 24,
    this.stepMs = 16,
    this.button = 'secondary',
  }) : super('DRAG_VIEWPORT_NATIVE');
}

class DragSplitHandleNative extends AutomationAction {
  final double targetFraction;
  final int steps;
  final int stepMs;

  const DragSplitHandleNative(
    this.targetFraction, {
    this.steps = 12,
    this.stepMs = 16,
  }) : super('DRAG_SPLIT_HANDLE_NATIVE');
}

class WheelViewportNative extends AutomationAction {
  final int delta;
  final int steps;
  final int stepMs;
  final double xFraction;
  final double yFraction;

  const WheelViewportNative(
    this.delta, {
    this.steps = 1,
    this.stepMs = 16,
    this.xFraction = 0.5,
    this.yFraction = 0.5,
  }) : super('WHEEL_VIEWPORT_NATIVE');
}

class DragViewportSampleOverlay extends AutomationAction {
  final double dx;
  final double dy;
  final int steps;
  final int stepMs;
  final double minScoreRatio;
  final int maxDropSamples;

  const DragViewportSampleOverlay(
    this.dx,
    this.dy, {
    this.steps = 24,
    this.stepMs = 16,
    this.minScoreRatio = 0.45,
    this.maxDropSamples = 0,
  }) : super('DRAG_VIEWPORT_SAMPLE_OVERLAY');
}

class DragViewportSampleNativeDiagnosticBool extends AutomationAction {
  final double dx;
  final double dy;
  final String key;
  final bool value;
  final int steps;
  final int stepMs;
  final int minMatches;

  const DragViewportSampleNativeDiagnosticBool(
    this.dx,
    this.dy, {
    required this.key,
    required this.value,
    this.steps = 24,
    this.stepMs = 16,
    this.minMatches = 1,
  }) : super('DRAG_VIEWPORT_SAMPLE_NATIVE_DIAGNOSTIC_BOOL');
}

class AssertViewportOverlayLineStyle extends AutomationAction {
  final int minPairedCenters;
  final double minPairedRatio;

  const AssertViewportOverlayLineStyle({
    this.minPairedCenters = 80,
    this.minPairedRatio = 0.72,
  }) : super('ASSERT_VIEWPORT_OVERLAY_LINE_STYLE');
}

class HoverControlsBarButtons extends AutomationAction {
  final int steps;

  const HoverControlsBarButtons({this.steps = 24})
    : super('HOVER_CONTROLS_BAR_BUTTONS');
}

class HoverControlsBarButtonsNative extends AutomationAction {
  final int steps;

  const HoverControlsBarButtonsNative({this.steps = 24})
    : super('HOVER_CONTROLS_BAR_BUTTONS_NATIVE');
}

class HoverTimeline extends AutomationAction {
  final int steps;
  final int stepMs;

  const HoverTimeline({this.steps = 48, this.stepMs = 8})
    : super('HOVER_TIMELINE');
}

class ClickMediaHeaderOverlayButtonNative extends AutomationAction {
  const ClickMediaHeaderOverlayButtonNative()
    : super('CLICK_MEDIA_HEADER_OVERLAY_BUTTON_NATIVE');
}

class ClickMediaHeaderOverlayButton extends AutomationAction {
  const ClickMediaHeaderOverlayButton()
    : super('CLICK_MEDIA_HEADER_OVERLAY_BUTTON');
}

class ClickMediaHeaderRemoveButton extends AutomationAction {
  final int fileId;

  const ClickMediaHeaderRemoveButton(this.fileId)
    : super('CLICK_MEDIA_HEADER_REMOVE_BUTTON');
}

class HoverMediaHeaderOverlayButton extends AutomationAction {
  const HoverMediaHeaderOverlayButton()
    : super('HOVER_MEDIA_HEADER_OVERLAY_BUTTON');
}

class HoverMediaHeaderOverlayPanelControls extends AutomationAction {
  const HoverMediaHeaderOverlayPanelControls()
    : super('HOVER_MEDIA_HEADER_OVERLAY_PANEL_CONTROLS');
}

class AssertMediaHeaderOverlayPanelVisible extends AutomationAction {
  final bool visible;

  const AssertMediaHeaderOverlayPanelVisible(this.visible)
    : super('ASSERT_MEDIA_HEADER_OVERLAY_PANEL_VISIBLE');
}

class ToggleAnalysisOverlay extends AutomationAction {
  final int slotIndex;

  const ToggleAnalysisOverlay(this.slotIndex)
    : super('TOGGLE_ANALYSIS_OVERLAY');
}

class ToggleAnalysisOverlayPanel extends AutomationAction {
  const ToggleAnalysisOverlayPanel() : super('TOGGLE_ANALYSIS_OVERLAY_PANEL');
}

class GenerateAnalysisCache extends AutomationAction {
  final int slotIndex;

  const GenerateAnalysisCache(this.slotIndex)
    : super('GENERATE_ANALYSIS_CACHE');
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

class ClearAnalysisChunks extends AutomationAction {
  const ClearAnalysisChunks() : super('CLEAR_ANALYSIS_CHUNKS');
}

/// Deletes every quick mark in the session and persists the empty set, so
/// scripted sessions start from a clean slate even when storage already has
/// marks for the same media content.
class ClearMarks extends AutomationAction {
  const ClearMarks() : super('CLEAR_MARKS');
}

class ToggleMarksSidebar extends AutomationAction {
  const ToggleMarksSidebar() : super('TOGGLE_MARKS_SIDEBAR');
}

/// Injects an agent-authored quick mark on the current frame of a slot:
/// a candidate region (normalized source coordinates) optionally tagged
/// with a defect type and severity for a human to confirm or reject.
class AddQuickMark extends AutomationAction {
  final int slotIndex;
  final double left;
  final double top;
  final double width;
  final double height;
  final String? defectType;
  final int? severity;

  const AddQuickMark({
    required this.slotIndex,
    required this.left,
    required this.top,
    required this.width,
    required this.height,
    this.defectType,
    this.severity,
  }) : super('ADD_QUICK_MARK');
}

/// Writes the verdict export document (all loaded media with lineage plus
/// in-memory marks including judgment fields) to a JSON file, so agents can
/// collect human verdicts from script-driven sessions.
class ExportMarks extends AutomationAction {
  final String outputPath;

  const ExportMarks(this.outputPath) : super('EXPORT_MARKS');
}

/// Declares the source lineage of a loaded track: which original clip the
/// media in [slotIndex] is an encode of. Agents use this when loading encode
/// candidates so annotations can be joined across encodes of the same source.
class SetMediaSourceId extends AutomationAction {
  final int slotIndex;
  final String sourceId;

  const SetMediaSourceId(this.slotIndex, this.sourceId)
    : super('SET_MEDIA_SOURCE_ID');
}

import '../actions/action_registry.dart';
import '../actions/automation_action.dart';
import '../actions/player_action.dart';
import '../analysis/analysis_overlay.dart';
import '../analysis/ui/testing/analysis_test_host.dart';
import '../video_renderer_controller.dart';
import 'main_window_harness.dart';

// ignore_for_file: prefer_initializing_formals

/// Explicit release UI automation bridge exposed by the main window.
///
/// This keeps automation wiring visible without making TestRunner part of the
/// user action binding lifecycle.
class UiAutomationBridge {
  final NativePlayerController controller;
  final MainWindowTestHarness testHarness;
  final int Function() effectiveDurationUs;
  final int Function() timelinePtsUs;
  final Future<void> Function(int slotIndex) toggleAnalysisOverlayForSlot;
  final Future<void> Function() toggleAnalysisOverlayPanel;
  final void Function() toggleMarksSidebar;
  final Future<String?> Function(int slotIndex) generateAnalysisCacheForSlot;
  final Future<void> Function(int slotIndex, String sourceId)
  setMediaSourceIdForSlot;
  final Future<void> Function(String outputPath) exportMarksToFile;
  final Future<void> Function(AddQuickMark action) addQuickMark;
  final void Function() clearMarks;
  final int Function() quickMarkCount;
  final void Function(AnalysisOverlayType type) setAnalysisOverlayType;
  final void Function(Set<AnalysisOverlayLayer> layers)
  setAnalysisOverlayLayers;
  final void Function(double opacity) setAnalysisOverlayOpacity;
  final Map<String, Object> Function() dartViewportDiagnostics;
  final AnalysisTestHostRegistry analysisTestHosts;
  final int Function() analysisEntryCount;
  final String Function() mainWindowDeckTabName;
  final bool Function() mainWindowDeckCollapsed;
  final ActionRegistry _actionRegistry;

  const UiAutomationBridge({
    required this.controller,
    required this.testHarness,
    required this.effectiveDurationUs,
    required this.timelinePtsUs,
    required this.toggleAnalysisOverlayForSlot,
    required this.toggleAnalysisOverlayPanel,
    required this.toggleMarksSidebar,
    required this.generateAnalysisCacheForSlot,
    required this.setMediaSourceIdForSlot,
    required this.exportMarksToFile,
    required this.addQuickMark,
    required this.clearMarks,
    required this.quickMarkCount,
    required this.setAnalysisOverlayType,
    required this.setAnalysisOverlayLayers,
    required this.setAnalysisOverlayOpacity,
    required this.dartViewportDiagnostics,
    required this.analysisTestHosts,
    required this.analysisEntryCount,
    required this.mainWindowDeckTabName,
    required this.mainWindowDeckCollapsed,
    required ActionRegistry actionRegistry,
  }) : _actionRegistry = actionRegistry;

  Future<void> executePlayerAction(PlayerAction action) {
    return _actionRegistry.executeAndWait(action.name, action);
  }
}

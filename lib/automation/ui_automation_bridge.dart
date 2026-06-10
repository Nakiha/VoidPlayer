import '../actions/action_registry.dart';
import '../actions/player_action.dart';
import '../analysis/analysis_overlay.dart';
import '../platform/analysis_process_host.dart';
import '../video_renderer_controller.dart';
import '../windows/main/main_window_test_hooks.dart';

// ignore_for_file: prefer_initializing_formals

/// Explicit release UI automation bridge exposed by the main window.
///
/// This keeps automation wiring visible without making TestRunner part of the
/// user action binding lifecycle.
class UiAutomationBridge {
  final NativePlayerController controller;
  final AnalysisProcessHost analysisProcesses;
  final MainWindowTestHarness testHarness;
  final int Function() effectiveDurationUs;
  final Future<void> Function(int slotIndex) toggleAnalysisOverlayForSlot;
  final Future<void> Function() toggleAnalysisOverlayPanel;
  final Future<String?> Function(int slotIndex) generateAnalysisCacheForSlot;
  final void Function(AnalysisOverlayType type) setAnalysisOverlayType;
  final void Function(Set<AnalysisOverlayLayer> layers)
  setAnalysisOverlayLayers;
  final void Function(double opacity) setAnalysisOverlayOpacity;
  final ActionRegistry _actionRegistry;

  const UiAutomationBridge({
    required this.controller,
    required this.analysisProcesses,
    required this.testHarness,
    required this.effectiveDurationUs,
    required this.toggleAnalysisOverlayForSlot,
    required this.toggleAnalysisOverlayPanel,
    required this.generateAnalysisCacheForSlot,
    required this.setAnalysisOverlayType,
    required this.setAnalysisOverlayLayers,
    required this.setAnalysisOverlayOpacity,
    required ActionRegistry actionRegistry,
  }) : _actionRegistry = actionRegistry;

  Future<void> executePlayerAction(PlayerAction action) {
    return _actionRegistry.executeAndWait(action.name, action);
  }

  Future<bool> waitForAnalysisProcessCount(int count, Duration timeout) =>
      analysisProcesses.waitForAnalysisProcessCount(count, timeout);

  int get analysisProcessCount => analysisProcesses.analysisProcessCount;

  Map<String, int> get analysisExitCodes => analysisProcesses.analysisExitCodes;

  set analysisTestScriptPath(String? value) {
    analysisProcesses.analysisTestScriptPath = value;
  }

  Future<void> closeAllAnalysisWindows() =>
      analysisProcesses.closeAllAnalysisWindows();
}

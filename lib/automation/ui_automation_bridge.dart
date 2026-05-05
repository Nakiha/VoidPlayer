import '../actions/action_registry.dart';
import '../actions/player_action.dart';
import '../video_renderer_controller.dart';
import '../windows/window_manager.dart';

/// Explicit release UI automation bridge exposed by the main window.
///
/// This keeps automation wiring visible without making TestRunner part of the
/// user action binding lifecycle.
class UiAutomationBridge {
  final NativePlayerController controller;
  final AnalysisProcessManager analysisProcesses;
  final ActionRegistry _actionRegistry;

  const UiAutomationBridge({
    required this.controller,
    required this.analysisProcesses,
    required ActionRegistry actionRegistry,
  }) : _actionRegistry = actionRegistry;

  void executePlayerAction(PlayerAction action) {
    _actionRegistry.execute(action.name, action);
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

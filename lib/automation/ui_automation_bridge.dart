import '../actions/action_registry.dart';
import '../actions/player_action.dart';
import '../video_renderer_controller.dart';

/// Explicit release UI automation bridge exposed by the main window.
///
/// This keeps automation wiring visible without making TestRunner part of the
/// user action binding lifecycle.
class UiAutomationBridge {
  final NativePlayerController controller;
  final ActionRegistry _actionRegistry;

  const UiAutomationBridge({
    required this.controller,
    required ActionRegistry actionRegistry,
  }) : _actionRegistry = actionRegistry;

  void executePlayerAction(PlayerAction action) {
    _actionRegistry.execute(action.name, action);
  }
}

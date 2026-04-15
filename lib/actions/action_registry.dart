import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_log.dart';
import 'player_action.dart';

/// Global action registry instance.
final actionRegistry = ActionRegistry();

/// Callback type for action handlers. Receives the action instance so
/// parameterized actions (e.g. [SeekTo], [SetSpeed]) can read their data.
typedef ActionCallback = void Function(PlayerAction action);

/// Central registry for player actions with keyboard interception.
///
/// [bind] registers an action definition + callback and starts intercepting
/// its shortcut key. [unbind] removes the callback and stops interception.
class ActionRegistry {
  final Map<String, PlayerAction> _actions = {};
  final Map<String, ActionCallback> _callbacks = {};
  final Map<LogicalKeyboardKey, String> _keyMap = {};
  final Set<LogicalKeyboardKey> _requireControl = {};

  /// Bind an action with its callback.
  ///
  /// If the action has a [PlayerAction.shortcut], that key will be intercepted
  /// by [ActionFocus] and routed to this callback.
  void bind(PlayerAction action, ActionCallback callback) {
    _actions[action.name] = action;
    _callbacks[action.name] = callback;
    if (action.shortcut != null) {
      _keyMap[action.shortcut!] = action.name;
      if (action.requireControl) {
        _requireControl.add(action.shortcut!);
      }
    }
  }

  /// Unbind an action by name.
  ///
  /// Removes the callback and the shortcut key mapping.
  void unbind(String name) {
    final action = _actions.remove(name);
    if (action?.shortcut != null) {
      _keyMap.remove(action!.shortcut);
      _requireControl.remove(action.shortcut);
    }
    _callbacks.remove(name);
  }

  /// Execute an action by name, optionally with an override action instance.
  ///
  /// When called from a keyboard shortcut, [overrideAction] is null and the
  /// registered default action is used. When called from a test script,
  /// [overrideAction] carries the script-specified parameters (e.g. seek position).
  void execute(String name, [PlayerAction? overrideAction]) {
    final callback = _callbacks[name];
    if (callback == null) {
      log.severe('Action "$name" not bound');
      return;
    }
    final action = overrideAction ?? _actions[name];
    log.info('Action: $name${action != overrideAction ? '' : ' (script)'}');
    callback(action!);
  }

  /// Handle a key event from [ActionFocus].
  ///
  /// Returns [KeyEventResult.handled] to swallow the key, or
  /// [KeyEventResult.ignored] to let Flutter process it normally.
  KeyEventResult handleKey(FocusNode node, KeyEvent event) {
    if (event is! KeyDownEvent) return KeyEventResult.ignored;

    // Pass through all keys when an EditableText has focus.
    if (_focusIsEditableText()) return KeyEventResult.ignored;

    final actionName = _keyMap[event.logicalKey];
    if (actionName == null) return KeyEventResult.ignored;

    // Check if this action requires Ctrl to be held
    final needsCtrl = _requireControl.contains(event.logicalKey);
    final ctrlHeld = HardwareKeyboard.instance.isControlPressed;
    if (needsCtrl != ctrlHeld) return KeyEventResult.ignored;

    final callback = _callbacks[actionName];
    if (callback == null) return KeyEventResult.ignored;

    execute(actionName);
    return KeyEventResult.handled;
  }

  bool _focusIsEditableText() {
    final primary = WidgetsBinding.instance.focusManager.primaryFocus;
    if (primary == null) return false;
    return primary.context?.widget is EditableText;
  }
}

/// A widget that intercepts registered shortcut keys globally.
///
/// Place this above your page content in the widget tree. It uses a [Focus]
/// widget with [autofocus] so it captures key events before Flutter's
/// default handlers (focus traversal, button activation, etc.).
class ActionFocus extends StatelessWidget {
  final Widget child;

  const ActionFocus({super.key, required this.child});

  @override
  Widget build(BuildContext context) {
    return Focus(
      autofocus: true,
      onKeyEvent: actionRegistry.handleKey,
      child: child,
    );
  }
}

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_log.dart';
import '../platform/keyboard_input_service.dart';
import '../utils/async_guard.dart';
import 'player_action.dart';

/// Callback type for action handlers. Receives the action instance so
/// parameterized actions (e.g. [SeekTo], [SetSpeed]) can read their data.
typedef ActionCallback = FutureOr<void> Function(PlayerAction action);

enum ShortcutTraceDecision {
  released,
  passedToEditable,
  modifierMismatch,
  swallowedRepeat,
  callbackMissing,
  dispatched,
}

class ShortcutTraceRecord {
  final int sequence;
  final int registryId;
  final String actionName;
  final String eventType;
  final String logicalKey;
  final String physicalKey;
  final bool synthesized;
  final bool controlPressed;
  final bool editable;
  final String focus;
  final ShortcutTraceDecision decision;

  const ShortcutTraceRecord({
    required this.sequence,
    required this.registryId,
    required this.actionName,
    required this.eventType,
    required this.logicalKey,
    required this.physicalKey,
    required this.synthesized,
    required this.controlPressed,
    required this.editable,
    required this.focus,
    required this.decision,
  });

  String toLogLine() =>
      '[ShortcutTrace][Flutter] seq=$sequence registry=$registryId '
      'action=$actionName event=$eventType logical=$logicalKey '
      'physical=$physicalKey synthesized=$synthesized ctrl=$controlPressed '
      'editable=$editable focus=$focus decision=${decision.name}';
}

typedef ShortcutTraceSink = void Function(ShortcutTraceRecord record);

void _logShortcutTrace(ShortcutTraceRecord record) {
  log.info(record.toLogLine());
}

/// Central registry for player actions with keyboard interception.
///
/// [bind] registers an action definition + callback and starts intercepting
/// its shortcut key. [unbind] removes the callback and stops interception.
class ActionRegistry {
  final KeyboardInputService keyboardInput;
  final ShortcutTraceSink shortcutTraceSink;
  final Map<String, PlayerAction> _actions = {};
  final Map<String, ActionCallback> _callbacks = {};
  final Map<LogicalKeyboardKey, String> _keyMap = {};
  final Set<LogicalKeyboardKey> _requireControl = {};
  int _shortcutTraceSequence = 0;

  ActionRegistry({
    this.keyboardInput = const FlutterKeyboardInputService(),
    ShortcutTraceSink? shortcutTraceSink,
  }) : shortcutTraceSink = shortcutTraceSink ?? _logShortcutTrace;

  /// Bind an action with its callback.
  ///
  /// If the action has a [PlayerAction.shortcut], that key will be intercepted
  /// by [ActionFocus] and routed to this callback.
  void bind(PlayerAction action, ActionCallback callback) {
    final old = _actions[action.name];
    if (old?.shortcut != null && old!.shortcut != action.shortcut) {
      if (_keyMap[old.shortcut] == action.name) {
        _keyMap.remove(old.shortcut);
        _requireControl.remove(old.shortcut);
      }
    }

    _actions[action.name] = action;
    _callbacks[action.name] = callback;
    if (action.shortcut != null) {
      final existing = _keyMap[action.shortcut!];
      if (existing != null && existing != action.name) {
        throw StateError(
          'Shortcut ${action.shortcut!.debugName ?? action.shortcut} '
          'already bound to $existing, cannot bind ${action.name}',
        );
      }
      _keyMap[action.shortcut!] = action.name;
      if (action.requireControl) {
        _requireControl.add(action.shortcut!);
      } else {
        _requireControl.remove(action.shortcut!);
      }
    }
  }

  /// Unbind an action by name.
  ///
  /// Removes the callback and the shortcut key mapping.
  void unbind(String name) {
    final action = _actions.remove(name);
    if (action?.shortcut != null) {
      if (_keyMap[action!.shortcut] == name) {
        _keyMap.remove(action.shortcut);
        _requireControl.remove(action.shortcut);
      }
    }
    _callbacks.remove(name);
  }

  /// Execute an action by name and log asynchronous failures.
  ///
  /// This keeps keyboard/button actions fire-and-forget while allowing
  /// automation to call [executeAndWait] for actions whose visual result is
  /// committed asynchronously, such as seek preview refresh.
  void execute(String name, [PlayerAction? overrideAction]) {
    fireAndLog('Action "$name"', executeAndWait(name, overrideAction));
  }

  /// Execute an action by name, optionally with an override action instance.
  ///
  /// When called from a keyboard shortcut, [overrideAction] is null and the
  /// registered default action is used. When called from a test script,
  /// [overrideAction] carries the script-specified parameters (e.g. seek position).
  Future<void> executeAndWait(
    String name, [
    PlayerAction? overrideAction,
  ]) async {
    final callback = _callbacks[name];
    if (callback == null) {
      log.severe('Action "$name" not bound');
      throw StateError('Action "$name" not bound');
    }
    final action = overrideAction ?? _actions[name];
    log.info('Action: $name${action != overrideAction ? '' : ' (script)'}');
    if (action == null) {
      throw StateError('Action "$name" has no registered definition');
    }
    final result = callback(action);
    if (result is Future) {
      await result;
    }
  }

  /// Handle a key event from [ActionFocus].
  ///
  /// Returns [KeyEventResult.handled] to swallow the key, or
  /// [KeyEventResult.ignored] to let Flutter process it normally.
  KeyEventResult handleKey(FocusNode node, KeyEvent event) {
    return handleKeyEvent(event)
        ? KeyEventResult.handled
        : KeyEventResult.ignored;
  }

  /// Handle a key event from Flutter's global keyboard dispatcher.
  bool handleKeyEvent(KeyEvent event) {
    final actionName = _keyMap[event.logicalKey];
    if (actionName == null) return false;

    final controlPressed = keyboardInput.isControlPressed;
    final editable = _focusIsEditableText();
    final focus = _primaryFocusDescription();
    void trace(ShortcutTraceDecision decision) {
      shortcutTraceSink(
        ShortcutTraceRecord(
          sequence: ++_shortcutTraceSequence,
          registryId: identityHashCode(this),
          actionName: actionName,
          eventType: switch (event) {
            KeyDownEvent() => 'down',
            KeyRepeatEvent() => 'repeat',
            KeyUpEvent() => 'up',
            _ => event.runtimeType.toString(),
          },
          logicalKey: event.logicalKey.debugName ?? '${event.logicalKey.keyId}',
          physicalKey:
              event.physicalKey.debugName ?? '${event.physicalKey.usbHidUsage}',
          synthesized: event.synthesized,
          controlPressed: controlPressed,
          editable: editable,
          focus: focus,
          decision: decision,
        ),
      );
    }

    if (event is! KeyDownEvent && event is! KeyRepeatEvent) {
      trace(ShortcutTraceDecision.released);
      return false;
    }

    // Pass through all keys when an EditableText has focus.
    if (editable) {
      trace(ShortcutTraceDecision.passedToEditable);
      return false;
    }
    final action = _actions[actionName];

    // Check if this action requires Ctrl to be held
    final needsCtrl = _requireControl.contains(event.logicalKey);
    if (needsCtrl != controlPressed) {
      trace(ShortcutTraceDecision.modifierMismatch);
      return false;
    }
    if (event is KeyRepeatEvent && action?.repeatable != true) {
      trace(ShortcutTraceDecision.swallowedRepeat);
      return true;
    }

    final callback = _callbacks[actionName];
    if (callback == null) {
      trace(ShortcutTraceDecision.callbackMissing);
      return false;
    }

    trace(ShortcutTraceDecision.dispatched);
    execute(actionName);
    return true;
  }

  String _primaryFocusDescription() {
    final primary = WidgetsBinding.instance.focusManager.primaryFocus;
    if (primary == null) return 'none';
    final label = primary.debugLabel ?? 'unlabelled';
    final widget = primary.context?.widget.runtimeType.toString() ?? 'detached';
    return '$label/$widget';
  }

  bool _focusIsEditableText() {
    final primary = WidgetsBinding.instance.focusManager.primaryFocus;
    if (primary == null) return false;
    final context = primary.context;
    if (context == null) return false;
    final widget = context.widget;
    if (widget is EditableText && widget.focusNode == primary) return true;
    final ancestor = context.findAncestorWidgetOfExactType<EditableText>();
    if (ancestor != null && ancestor.focusNode == primary) {
      return true;
    }
    if (context is! Element) return false;

    var found = false;
    void visit(Element element) {
      if (found) return;
      final widget = element.widget;
      if (widget is EditableText && widget.focusNode == primary) {
        found = true;
        return;
      }
      element.visitChildElements(visit);
    }

    context.visitChildElements(visit);
    return found;
  }
}

/// A widget that intercepts registered shortcut keys globally.
///
/// Place this above your page content in the widget tree. It registers with
/// the registry's [KeyboardInputService] so window-level shortcuts keep working
/// even if native fullscreen transitions or overlays temporarily move primary
/// focus away from this subtree.
class ActionFocus extends StatefulWidget {
  final ActionRegistry actionRegistry;
  final Widget child;

  const ActionFocus({
    super.key,
    required this.actionRegistry,
    required this.child,
  });

  @override
  State<ActionFocus> createState() => _ActionFocusState();
}

class _ActionFocusState extends State<ActionFocus> {
  late final FocusNode _focusNode = FocusNode(debugLabel: 'ActionFocus');
  late KeyboardInputService _registeredKeyboardInput;

  @override
  void initState() {
    super.initState();
    _registeredKeyboardInput = widget.actionRegistry.keyboardInput;
    _registeredKeyboardInput.addHandler(_handleGlobalKeyEvent);
    _logHandlerLifecycle('registered');
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || _focusNode.hasFocus) return;
      _focusNode.requestFocus();
    });
  }

  @override
  void didUpdateWidget(covariant ActionFocus oldWidget) {
    super.didUpdateWidget(oldWidget);
    final nextKeyboardInput = widget.actionRegistry.keyboardInput;
    if (identical(_registeredKeyboardInput, nextKeyboardInput)) return;
    _registeredKeyboardInput.removeHandler(_handleGlobalKeyEvent);
    _logHandlerLifecycle('moved-from');
    _registeredKeyboardInput = nextKeyboardInput;
    _registeredKeyboardInput.addHandler(_handleGlobalKeyEvent);
    _logHandlerLifecycle('moved-to');
  }

  @override
  void dispose() {
    _registeredKeyboardInput.removeHandler(_handleGlobalKeyEvent);
    _logHandlerLifecycle('removed');
    _focusNode.dispose();
    super.dispose();
  }

  bool _handleGlobalKeyEvent(KeyEvent event) {
    return widget.actionRegistry.handleKeyEvent(event);
  }

  void _logHandlerLifecycle(String state) {
    log.info(
      '[ShortcutTrace][Flutter] handler=$state '
      'registry=${identityHashCode(widget.actionRegistry)} '
      'input=${identityHashCode(_registeredKeyboardInput)}',
    );
  }

  @override
  Widget build(BuildContext context) {
    return Focus(focusNode: _focusNode, autofocus: true, child: widget.child);
  }
}

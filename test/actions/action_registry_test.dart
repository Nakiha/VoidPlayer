import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/actions/action_registry.dart';
import 'package:void_player/actions/player_action.dart';
import 'package:void_player/app_log.dart';
import 'package:void_player/platform/keyboard_input_service.dart';

void main() {
  setUpAll(() async {
    await initLogging(const []);
  });

  tearDownAll(shutdownLogging);

  test('shortcut display metadata is derived from real action definitions', () {
    final entries = PlayerAction.shortcutEntries;

    expect(
      entries.map((entry) => entry.actionName),
      orderedEquals([
        'TOGGLE_PLAY_PAUSE',
        'STEP_FORWARD',
        'STEP_BACKWARD',
        'OPEN_FILE',
        'TOGGLE_LAYOUT_MODE',
        'TOGGLE_FULL_SCREEN',
        'EXIT_FULL_SCREEN',
      ]),
    );
    expect(
      entries.map((entry) => entry.shortcutLabel),
      orderedEquals(['Space', '→', '←', 'O', 'M', 'F11', 'Esc']),
    );
    expect(
      entries.map((entry) => entry.labelKey),
      isNot(contains('actionSeekForward')),
    );
  });

  testWidgets('shortcuts work when an unfocused text field is in the subtree', (
    tester,
  ) async {
    final actionRegistry = ActionRegistry();
    var triggerCount = 0;
    actionRegistry.bind(const ToggleLayoutMode(), (_) {
      triggerCount++;
    });

    await tester.pumpWidget(
      MaterialApp(
        home: ActionFocus(
          actionRegistry: actionRegistry,
          child: const Scaffold(body: TextField()),
        ),
      ),
    );
    await tester.pump();

    await tester.sendKeyDownEvent(LogicalKeyboardKey.keyM);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.keyM);

    expect(triggerCount, 1);
  });

  testWidgets('shortcuts work when primary focus leaves ActionFocus subtree', (
    tester,
  ) async {
    final actionRegistry = ActionRegistry();
    var triggerCount = 0;
    actionRegistry.bind(const ToggleLayoutMode(), (_) {
      triggerCount++;
    });
    final outsideFocusNode = FocusNode();
    addTearDown(outsideFocusNode.dispose);

    await tester.pumpWidget(
      MaterialApp(
        home: Row(
          children: [
            ActionFocus(
              actionRegistry: actionRegistry,
              child: const SizedBox.shrink(),
            ),
            Focus(
              focusNode: outsideFocusNode,
              child: const SizedBox(width: 10, height: 10),
            ),
          ],
        ),
      ),
    );
    outsideFocusNode.requestFocus();
    await tester.pump();

    await tester.sendKeyDownEvent(LogicalKeyboardKey.keyM);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.keyM);

    expect(triggerCount, 1);
  });

  testWidgets('shortcuts pass through while editing text', (tester) async {
    final actionRegistry = ActionRegistry();
    var triggerCount = 0;
    actionRegistry.bind(const ToggleLayoutMode(), (_) {
      triggerCount++;
    });

    await tester.pumpWidget(
      MaterialApp(
        home: ActionFocus(
          actionRegistry: actionRegistry,
          child: const Scaffold(body: TextField()),
        ),
      ),
    );
    await tester.tap(find.byType(TextField));
    await tester.pump();

    await tester.sendKeyDownEvent(LogicalKeyboardKey.keyM);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.keyM);

    expect(triggerCount, 0);
  });

  testWidgets('non-repeatable shortcuts swallow key repeats without firing', (
    tester,
  ) async {
    final actionRegistry = ActionRegistry();
    var toggleCount = 0;
    var stepCount = 0;
    actionRegistry.bind(const TogglePlayPause(), (_) {
      toggleCount++;
    });
    actionRegistry.bind(const StepForward(), (_) {
      stepCount++;
    });

    await tester.pumpWidget(
      MaterialApp(
        home: ActionFocus(
          actionRegistry: actionRegistry,
          child: const SizedBox.shrink(),
        ),
      ),
    );
    await tester.sendKeyDownEvent(LogicalKeyboardKey.space);
    await tester.sendKeyRepeatEvent(LogicalKeyboardKey.space);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.space);
    await tester.sendKeyDownEvent(LogicalKeyboardKey.arrowRight);
    await tester.sendKeyRepeatEvent(LogicalKeyboardKey.arrowRight);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.arrowRight);

    expect(toggleCount, 1);
    expect(stepCount, 2);
  });

  testWidgets('shortcut trace records dispatch and release with focus owner', (
    tester,
  ) async {
    final traces = <ShortcutTraceRecord>[];
    final actionRegistry = ActionRegistry(shortcutTraceSink: traces.add);
    var toggleCount = 0;
    actionRegistry.bind(const TogglePlayPause(), (_) => toggleCount++);

    await tester.pumpWidget(
      MaterialApp(
        home: ActionFocus(
          actionRegistry: actionRegistry,
          child: const SizedBox.shrink(),
        ),
      ),
    );
    await tester.sendKeyDownEvent(LogicalKeyboardKey.space);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.space);

    expect(toggleCount, 1);
    expect(
      traces.map((trace) => trace.decision),
      orderedEquals([
        ShortcutTraceDecision.dispatched,
        ShortcutTraceDecision.released,
      ]),
    );
    expect(traces.first.actionName, 'TOGGLE_PLAY_PAUSE');
    expect(traces.first.eventType, 'down');
    expect(traces.first.focus, contains('ActionFocus'));
  });

  testWidgets('shortcut trace identifies EditableText pass-through', (
    tester,
  ) async {
    final traces = <ShortcutTraceRecord>[];
    final actionRegistry = ActionRegistry(shortcutTraceSink: traces.add);
    actionRegistry.bind(const TogglePlayPause(), (_) {});

    await tester.pumpWidget(
      MaterialApp(
        home: ActionFocus(
          actionRegistry: actionRegistry,
          child: const Scaffold(body: TextField()),
        ),
      ),
    );
    await tester.tap(find.byType(TextField));
    await tester.sendKeyDownEvent(LogicalKeyboardKey.space);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.space);

    expect(
      traces.map((trace) => trace.decision),
      orderedEquals([
        ShortcutTraceDecision.passedToEditable,
        ShortcutTraceDecision.released,
      ]),
    );
    expect(traces.first.editable, isTrue);
  });

  testWidgets(
    'Windows runner owns application shortcuts without double dispatch',
    (tester) async {
      final traces = <ShortcutTraceRecord>[];
      final actionRegistry = ActionRegistry(
        useWindowsRunnerShortcuts: true,
        shortcutTraceSink: traces.add,
      );
      var toggleCount = 0;
      actionRegistry.bind(const TogglePlayPause(), (_) => toggleCount++);

      await tester.pumpWidget(
        MaterialApp(
          home: ActionFocus(
            actionRegistry: actionRegistry,
            child: const SizedBox.shrink(),
          ),
        ),
      );
      await tester.sendKeyDownEvent(LogicalKeyboardKey.space);
      expect(toggleCount, 0);
      expect(traces.last.decision, ShortcutTraceDecision.platformOwned);

      expect(
        actionRegistry.handleWindowsRunnerShortcut(
          virtualKey: 0x20,
          scanCode: 0x39,
          repeat: false,
          controlPressed: false,
        ),
        isTrue,
      );
      expect(toggleCount, 1);
      expect(traces.last.decision, ShortcutTraceDecision.dispatched);

      actionRegistry.handleWindowsRunnerShortcut(
        virtualKey: 0x20,
        scanCode: 0x39,
        repeat: true,
        controlPressed: false,
      );
      expect(toggleCount, 1);
      expect(traces.last.decision, ShortcutTraceDecision.swallowedRepeat);
    },
  );

  testWidgets('Windows runner shortcuts pass through while editing text', (
    tester,
  ) async {
    final actionRegistry = ActionRegistry(useWindowsRunnerShortcuts: true);
    var toggleCount = 0;
    actionRegistry.bind(const TogglePlayPause(), (_) => toggleCount++);

    await tester.pumpWidget(
      MaterialApp(
        home: ActionFocus(
          actionRegistry: actionRegistry,
          child: const Scaffold(body: TextField()),
        ),
      ),
    );
    await tester.tap(find.byType(TextField));
    await tester.pump();

    expect(
      actionRegistry.handleWindowsRunnerShortcut(
        virtualKey: 0x20,
        scanCode: 0x39,
        repeat: false,
        controlPressed: false,
      ),
      isFalse,
    );
    expect(toggleCount, 0);
  });

  testWidgets('ActionFocus moves its global handler with keyboard service', (
    tester,
  ) async {
    final firstInput = _FakeKeyboardInputService();
    final secondInput = _FakeKeyboardInputService();
    final firstRegistry = ActionRegistry(keyboardInput: firstInput);
    final secondRegistry = ActionRegistry(keyboardInput: secondInput);
    var secondCount = 0;
    firstRegistry.bind(const ToggleLayoutMode(), (_) {});
    secondRegistry.bind(const ToggleLayoutMode(), (_) => secondCount++);

    await tester.pumpWidget(
      MaterialApp(
        home: ActionFocus(
          actionRegistry: firstRegistry,
          child: const SizedBox.shrink(),
        ),
      ),
    );
    expect(firstInput.handlerCount, 1);
    expect(secondInput.handlerCount, 0);

    await tester.pumpWidget(
      MaterialApp(
        home: ActionFocus(
          actionRegistry: secondRegistry,
          child: const SizedBox.shrink(),
        ),
      ),
    );
    expect(firstInput.handlerCount, 0);
    expect(secondInput.handlerCount, 1);

    secondInput.dispatch(
      const KeyDownEvent(
        physicalKey: PhysicalKeyboardKey.keyM,
        logicalKey: LogicalKeyboardKey.keyM,
        timeStamp: Duration.zero,
      ),
    );
    expect(secondCount, 1);
  });
}

class _FakeKeyboardInputService implements KeyboardInputService {
  final List<KeyboardEventHandler> _handlers = [];

  int get handlerCount => _handlers.length;

  @override
  bool isControlPressed = false;

  @override
  void addHandler(KeyboardEventHandler handler) => _handlers.add(handler);

  @override
  void removeHandler(KeyboardEventHandler handler) => _handlers.remove(handler);

  bool dispatch(KeyEvent event) {
    var handled = false;
    for (final handler in List<KeyboardEventHandler>.of(_handlers)) {
      handled = handler(event) || handled;
    }
    return handled;
  }
}

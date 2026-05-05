import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/actions/action_registry.dart';
import 'package:void_player/actions/player_action.dart';
import 'package:void_player/app_log.dart';

void main() {
  setUpAll(() async {
    await initLogging(const []);
  });

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
}

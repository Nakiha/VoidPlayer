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

  tearDown(() {
    actionRegistry.unbind(const ToggleLayoutMode().name);
    actionRegistry.unbind(const TogglePlayPause().name);
    actionRegistry.unbind(const StepForward().name);
  });

  testWidgets('shortcuts work when an unfocused text field is in the subtree', (
    tester,
  ) async {
    var triggerCount = 0;
    actionRegistry.bind(const ToggleLayoutMode(), (_) {
      triggerCount++;
    });

    await tester.pumpWidget(
      const MaterialApp(
        home: ActionFocus(child: Scaffold(body: TextField())),
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
            const ActionFocus(child: SizedBox.shrink()),
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
    var triggerCount = 0;
    actionRegistry.bind(const ToggleLayoutMode(), (_) {
      triggerCount++;
    });

    await tester.pumpWidget(
      const MaterialApp(
        home: ActionFocus(child: Scaffold(body: TextField())),
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
    var toggleCount = 0;
    var stepCount = 0;
    actionRegistry.bind(const TogglePlayPause(), (_) {
      toggleCount++;
    });
    actionRegistry.bind(const StepForward(), (_) {
      stepCount++;
    });

    await tester.pumpWidget(
      const MaterialApp(home: ActionFocus(child: SizedBox.shrink())),
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

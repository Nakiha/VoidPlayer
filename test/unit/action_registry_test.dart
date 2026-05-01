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
}

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/widgets/app_menu_combo.dart';

void main() {
  testWidgets('escape closes an open app menu combo', (tester) async {
    var selected = 'One';
    await tester.pumpWidget(
      _ComboHost(value: selected, onChanged: (value) => selected = value),
    );

    await tester.tap(find.byType(AppMenuCombo<String>));
    await tester.pumpAndSettle();
    expect(find.text('Two'), findsOneWidget);

    await tester.sendKeyEvent(LogicalKeyboardKey.escape);
    await tester.pumpAndSettle();

    expect(selected, 'One');
    expect(find.text('Two'), findsNothing);
  });

  testWidgets('keyboard can highlight and select an app menu combo item', (
    tester,
  ) async {
    var selected = 'One';
    await tester.pumpWidget(
      _ComboHost(
        value: selected,
        onChanged: (value) {
          selected = value;
        },
      ),
    );

    await tester.tap(find.byType(AppMenuCombo<String>));
    await tester.pumpAndSettle();
    await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pumpAndSettle();

    expect(selected, 'Two');
  });

  testWidgets('custom button and menu item builders use shared menu behavior', (
    tester,
  ) async {
    var selected = 'One';
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Align(
            alignment: Alignment.topLeft,
            child: AppMenuCombo<String>(
              value: selected,
              width: 160,
              minMenuWidth: 140,
              items: const ['One', 'Two'],
              labelFor: (value) => value,
              onChanged: (value) {
                selected = value;
              },
              buttonBuilder: (context, value, open) => Row(
                children: [
                  Expanded(
                    child: Text('Button $value', overflow: TextOverflow.clip),
                  ),
                  AppMenuComboArrow(open: open, size: 14),
                ],
              ),
              itemBuilder: (context, value, label, selected) => Row(
                children: [
                  if (selected) const Icon(Icons.check, size: 16),
                  Expanded(
                    child: Text('Custom $label', overflow: TextOverflow.clip),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );

    expect(find.text('Button One'), findsOneWidget);

    await tester.tap(find.byType(AppMenuCombo<String>));
    await tester.pumpAndSettle();
    expect(find.text('Custom Two'), findsOneWidget);

    await tester.tap(find.text('Custom Two'));
    await tester.pumpAndSettle();

    expect(selected, 'Two');
  });
}

class _ComboHost extends StatefulWidget {
  final String value;
  final ValueChanged<String> onChanged;

  const _ComboHost({required this.value, required this.onChanged});

  @override
  State<_ComboHost> createState() => _ComboHostState();
}

class _ComboHostState extends State<_ComboHost> {
  late String _value = widget.value;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        body: Align(
          alignment: Alignment.topLeft,
          child: AppMenuCombo<String>(
            value: _value,
            width: 160,
            items: const ['One', 'Two', 'Three'],
            labelFor: (value) => value,
            onChanged: (value) {
              setState(() => _value = value);
              widget.onChanged(value);
            },
          ),
        ),
      ),
    );
  }
}

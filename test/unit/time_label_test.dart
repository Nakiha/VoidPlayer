import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/widgets/time_label.dart';

void main() {
  test('parseEditableTimeUs accepts seconds and clock time', () {
    expect(parseEditableTimeUs('1.25'), 1250000);
    expect(parseEditableTimeUs('+00:01.250'), 1250000);
    expect(parseEditableTimeUs('02:03.004'), 123004000);
    expect(parseEditableTimeUs('1:02:03.5'), 3723500000);
    expect(parseEditableTimeUs('-0.5'), -500000);
  });

  test('parseEditableTimeUs rejects malformed time', () {
    expect(parseEditableTimeUs(''), isNull);
    expect(parseEditableTimeUs('+'), isNull);
    expect(parseEditableTimeUs('1::2'), isNull);
    expect(parseEditableTimeUs('1.2:03'), isNull);
    expect(parseEditableTimeUs('00:60'), isNull);
  });

  test(
    'EditableTimeInputFormatter allows only a leading sign and time chars',
    () {
      const formatter = EditableTimeInputFormatter();

      TextEditingValue edit(String oldText, String newText) {
        return formatter.formatEditUpdate(
          TextEditingValue(text: oldText),
          TextEditingValue(text: newText),
        );
      }

      expect(edit('12', '12:03.4').text, '12:03.4');
      expect(edit('', '-12').text, '-12');
      expect(edit('12', '12-3').text, '12');
      expect(edit('12', '12a').text, '12');
    },
  );

  testWidgets('EditableTimeLabel seeks on valid submitted input', (
    tester,
  ) async {
    final seeks = <int>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: EditableTimeLabel(
            currentUs: 0,
            totalUs: 10000000,
            onSeek: seeks.add,
          ),
        ),
      ),
    );

    await tester.tap(find.byType(TextField));
    await tester.enterText(find.byType(TextField), '00:01.250');
    await tester.testTextInput.receiveAction(TextInputAction.done);
    await tester.pump();

    expect(seeks, [1250000]);
    expect(
      tester.widget<TextField>(find.byType(TextField)).controller!.text,
      '00:01.250',
    );
  });

  testWidgets('EditableTimeLabel restores current time on invalid input', (
    tester,
  ) async {
    final seeks = <int>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: EditableTimeLabel(
            currentUs: 3456000,
            totalUs: 10000000,
            onSeek: seeks.add,
          ),
        ),
      ),
    );

    await tester.tap(find.byType(TextField));
    await tester.enterText(find.byType(TextField), '1::2');
    await tester.testTextInput.receiveAction(TextInputAction.done);
    await tester.pump();

    expect(seeks, isEmpty);
    expect(
      tester.widget<TextField>(find.byType(TextField)).controller!.text,
      '00:03.456',
    );
  });

  testWidgets('EditableTimeLabel clamps input to active seek range', (
    tester,
  ) async {
    final seeks = <int>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: EditableTimeLabel(
            currentUs: 0,
            totalUs: 10000000,
            seekMinUs: 2000000,
            seekMaxUs: 5000000,
            onSeek: seeks.add,
          ),
        ),
      ),
    );

    await tester.tap(find.byType(TextField));
    await tester.enterText(find.byType(TextField), '00:09.000');
    await tester.testTextInput.receiveAction(TextInputAction.done);
    await tester.pump();

    expect(seeks, [5000000]);
    expect(
      tester.widget<TextField>(find.byType(TextField)).controller!.text,
      '00:05.000',
    );
  });
}

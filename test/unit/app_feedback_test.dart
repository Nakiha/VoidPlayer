import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/feedback/app_feedback.dart';
import 'package:void_player/widgets/app_feedback_host.dart';

void main() {
  test('controller replaces and dismisses feedback', () {
    final controller = AppFeedbackController();
    addTearDown(controller.dispose);

    controller.showInfo('Ready');
    expect(controller.current?.text, 'Ready');
    expect(controller.current?.severity, AppFeedbackSeverity.info);

    controller.showError('Failed');
    expect(controller.current?.text, 'Failed');
    expect(controller.current?.severity, AppFeedbackSeverity.error);

    controller.dismiss();
    expect(controller.current, isNull);
  });

  testWidgets('host displays and dismisses feedback', (tester) async {
    final controller = AppFeedbackController();
    addTearDown(controller.dispose);

    await tester.pumpWidget(
      AppFeedbackScope(
        controller: controller,
        child: const MaterialApp(
          home: Scaffold(body: Stack(children: [AppFeedbackHost()])),
        ),
      ),
    );

    controller.show(
      const AppFeedbackMessage(
        text: 'Saved',
        severity: AppFeedbackSeverity.success,
        duration: Duration.zero,
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 200));

    expect(find.text('Saved'), findsOneWidget);

    await tester.tap(find.byIcon(Icons.close));
    await tester.pumpAndSettle();

    expect(find.text('Saved'), findsNothing);
  });
}

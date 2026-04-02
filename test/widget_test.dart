import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:void_player/main.dart';

void main() {
  testWidgets('App renders without crashing', (WidgetTester tester) async {
    await tester.pumpWidget(MyApp(accentColor: const Color(0xFF0078D4)));
    expect(find.text('Void Player'), findsOneWidget);
  });
}

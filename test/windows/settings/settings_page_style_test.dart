import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/windows/settings/settings_page_style.dart';

void main() {
  testWidgets('settings combo row stays inline at compact settings width', (
    tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Center(
            child: SizedBox(
              width: 390,
              child: SettingsComboRow<String>(
                label: '跳转画面后播放器行为',
                icon: Icons.slow_motion_video,
                value: '保持跳转前状态',
                items: const ['保持跳转前状态', '暂停'],
                labelFor: (value) => value,
                onChanged: (_) {},
              ),
            ),
          ),
        ),
      ),
    );

    final labelTop = tester.getTopLeft(find.text('跳转画面后播放器行为'));
    final valueTop = tester.getTopLeft(find.text('保持跳转前状态'));

    expect(valueTop.dx, greaterThan(labelTop.dx));
    expect((valueTop.dy - labelTop.dy).abs(), lessThan(10));
  });
}

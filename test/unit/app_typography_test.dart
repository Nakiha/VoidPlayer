import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/theme/app_typography.dart';

void main() {
  test('Windows keeps Segoe UI and Microsoft Chinese fallbacks', () {
    final typography = AppTypography.forPlatform(TargetPlatform.windows);

    expect(typography.fontFamily, 'Segoe UI');
    expect(typography.fontFamilyFallback, contains('Microsoft YaHei UI'));
  });

  test('macOS uses platform default primary font with macOS fallbacks', () {
    final typography = AppTypography.forPlatform(TargetPlatform.macOS);

    expect(typography.fontFamily, isNull);
    expect(typography.fontFamilyFallback, contains('PingFang SC'));
  });
}

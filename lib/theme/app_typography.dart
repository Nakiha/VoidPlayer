import 'package:flutter/foundation.dart';

class AppTypography {
  final String? fontFamily;
  final List<String>? fontFamilyFallback;

  const AppTypography({
    required this.fontFamily,
    required this.fontFamilyFallback,
  });

  static const _windows = AppTypography(
    fontFamily: 'Segoe UI',
    fontFamilyFallback: [
      'Microsoft YaHei UI',
      'Microsoft YaHei',
      'Microsoft JhengHei UI',
      'Microsoft JhengHei',
      'SimSun',
    ],
  );

  static const _macOS = AppTypography(
    fontFamily: null,
    fontFamilyFallback: [
      'PingFang SC',
      'Hiragino Sans GB',
      'Heiti SC',
      'Helvetica Neue',
    ],
  );

  static AppTypography forPlatform(TargetPlatform platform) {
    return switch (platform) {
      TargetPlatform.macOS => _macOS,
      TargetPlatform.windows => _windows,
      _ => const AppTypography(fontFamily: null, fontFamilyFallback: null),
    };
  }
}

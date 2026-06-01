import 'dart:ui';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/config/app_settings_repository.dart';
import 'package:void_player/preferences/playback_preferences.dart';
import 'package:void_player/theme/app_appearance.dart';

class _FakeAppSettingsRepository implements AppSettingsRepository {
  @override
  Rect? windowRect;

  @override
  int analysisCacheMaxBytes = 0;

  @override
  String themeModePreference = 'system';

  @override
  String accentColorPreference = 'system';

  @override
  int customAccentColorValue = 0xFF0078D4;

  @override
  SeekAfterJumpBehavior seekAfterJumpBehavior =
      SeekAfterJumpBehavior.keepPreviousState;

  @override
  DecodeMode decodeMode = DecodeMode.preferHardware;

  @override
  ViewportPixelSizeMode viewportPixelSizeMode =
      ViewportPixelSizeMode.uniformVideoPixels;

  @override
  Map<String, String> securityScopedBookmarks = {};

  @override
  Future<void> save() => Future.value();
}

void main() {
  test('system accent updates notify when following system color', () {
    final controller = AppAppearanceController.load(
      settings: _FakeAppSettingsRepository(),
      systemAccentColor: const Color(0xFF0078D4),
    );
    addTearDown(controller.dispose);

    var notifications = 0;
    controller.addListener(() => notifications++);

    controller.setSystemAccentColor(const Color(0xFFCA5010));

    expect(controller.systemAccentColor, const Color(0xFFCA5010));
    expect(controller.accentColor, const Color(0xFFCA5010));
    expect(notifications, 1);
  });

  test(
    'system accent updates are cached while custom color is active',
    () async {
      final settings = _FakeAppSettingsRepository()
        ..accentColorPreference = 'custom'
        ..customAccentColorValue = 0xFF00AA55;
      final controller = AppAppearanceController.load(
        settings: settings,
        systemAccentColor: const Color(0xFF0078D4),
      );
      addTearDown(controller.dispose);

      var notifications = 0;
      controller.addListener(() => notifications++);

      controller.setSystemAccentColor(const Color(0xFFCA5010));

      expect(controller.systemAccentColor, const Color(0xFFCA5010));
      expect(controller.accentColor, const Color(0xFF00AA55));
      expect(notifications, 0);

      await controller.setAccentPreference(AppAccentPreference.system);

      expect(controller.accentColor, const Color(0xFFCA5010));
      expect(notifications, 1);
    },
  );
}

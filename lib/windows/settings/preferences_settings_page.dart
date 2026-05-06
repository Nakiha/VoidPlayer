import 'dart:async';

import 'package:flutter/material.dart';

import '../../l10n/app_localizations.dart';
import '../../preferences/playback_preferences.dart';
import '../../theme/app_appearance.dart';
import 'settings_page_style.dart';

class PreferencesSettingsPage extends StatefulWidget {
  final ValueChanged<ViewportPixelSizeMode>? onViewportPixelSizeModeChanged;

  const PreferencesSettingsPage({
    super.key,
    this.onViewportPixelSizeModeChanged,
  });

  @override
  State<PreferencesSettingsPage> createState() =>
      _PreferencesSettingsPageState();
}

class _PreferencesSettingsPageState extends State<PreferencesSettingsPage> {
  late SeekAfterJumpBehavior _seekBehavior;
  late DecodeMode _decodeMode;
  late ViewportPixelSizeMode _pixelSizeMode;

  @override
  void initState() {
    super.initState();
    final settings = AppSettingsScope.read(context);
    _seekBehavior = settings.seekAfterJumpBehavior;
    _decodeMode = settings.decodeMode;
    _pixelSizeMode = settings.viewportPixelSizeMode;
  }

  Future<void> _setSeekBehavior(SeekAfterJumpBehavior behavior) async {
    if (_seekBehavior == behavior) return;
    setState(() => _seekBehavior = behavior);
    final settings = AppSettingsScope.of(context);
    settings.seekAfterJumpBehavior = behavior;
    await settings.save();
  }

  Future<void> _setDecodeMode(DecodeMode mode) async {
    if (_decodeMode == mode) return;
    setState(() => _decodeMode = mode);
    final settings = AppSettingsScope.of(context);
    settings.decodeMode = mode;
    await settings.save();
  }

  Future<void> _setPixelSizeMode(ViewportPixelSizeMode mode) async {
    if (_pixelSizeMode == mode) return;
    setState(() => _pixelSizeMode = mode);
    widget.onViewportPixelSizeModeChanged?.call(mode);
    final settings = AppSettingsScope.of(context);
    settings.viewportPixelSizeMode = mode;
    await settings.save();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return SingleChildScrollView(
      padding: SettingsPageStyle.pagePadding,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          SettingsPageTitle(text: l.preferences),
          SettingsPageStyle.contentGap,
          SettingsComboRow<SeekAfterJumpBehavior>(
            label: l.seekAfterJumpBehavior,
            icon: Icons.slow_motion_video,
            value: _seekBehavior,
            items: SeekAfterJumpBehavior.values,
            labelFor: (value) => switch (value) {
              SeekAfterJumpBehavior.forcePause => l.seekBehaviorForcePause,
              SeekAfterJumpBehavior.keepPreviousState =>
                l.seekBehaviorKeepPreviousState,
            },
            onChanged: (value) {
              unawaited(_setSeekBehavior(value));
            },
          ),
          SettingsPageStyle.contentGap,
          SettingsComboRow<ViewportPixelSizeMode>(
            label: l.viewportPixelSize,
            icon: Icons.aspect_ratio,
            value: _pixelSizeMode,
            items: ViewportPixelSizeMode.values,
            labelFor: (value) => switch (value) {
              ViewportPixelSizeMode.uniformVideoPixels =>
                l.viewportPixelSizeUniformVideoPixels,
              ViewportPixelSizeMode.fillView => l.viewportPixelSizeFillView,
            },
            onChanged: (value) {
              unawaited(_setPixelSizeMode(value));
            },
          ),
          SettingsPageStyle.contentGap,
          SettingsComboRow<DecodeMode>(
            label: l.decodeMode,
            icon: Icons.memory,
            value: _decodeMode,
            items: DecodeMode.values,
            labelFor: (value) => switch (value) {
              DecodeMode.preferHardware => l.decodeModePreferHardware,
              DecodeMode.forceSoftware => l.decodeModeForceSoftware,
            },
            onChanged: (value) {
              unawaited(_setDecodeMode(value));
            },
          ),
        ],
      ),
    );
  }
}

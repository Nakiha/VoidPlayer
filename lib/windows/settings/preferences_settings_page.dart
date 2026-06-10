import 'package:flutter/material.dart';

import '../../l10n/app_localizations.dart';
import '../../preferences/playback_preferences.dart';
import '../../theme/app_appearance.dart';
import '../../utils/async_guard.dart';
import 'settings_page_style.dart';

class PreferencesSettingsPage extends StatefulWidget {
  final ValueChanged<ViewportPixelSizeMode>? onViewportPixelSizeModeChanged;
  final ValueChanged<PerformanceAlertPolicy>? onPerformanceAlertPolicyChanged;

  const PreferencesSettingsPage({
    super.key,
    this.onViewportPixelSizeModeChanged,
    this.onPerformanceAlertPolicyChanged,
  });

  @override
  State<PreferencesSettingsPage> createState() =>
      _PreferencesSettingsPageState();
}

class _PreferencesSettingsPageState extends State<PreferencesSettingsPage> {
  late SeekAfterJumpBehavior _seekBehavior;
  late DecodeMode _decodeMode;
  late ViewportPixelSizeMode _pixelSizeMode;
  late DefaultAudioPlaybackPolicy _defaultAudioPolicy;
  late PerformanceAlertPolicy _performanceAlertPolicy;

  @override
  void initState() {
    super.initState();
    final settings = AppSettingsScope.read(context);
    _seekBehavior = settings.seekAfterJumpBehavior;
    _decodeMode = settings.decodeMode;
    _pixelSizeMode = settings.viewportPixelSizeMode;
    _defaultAudioPolicy = settings.defaultAudioPlaybackPolicy;
    _performanceAlertPolicy = settings.performanceAlertPolicy;
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

  Future<void> _setDefaultAudioPolicy(DefaultAudioPlaybackPolicy policy) async {
    if (_defaultAudioPolicy == policy) return;
    setState(() => _defaultAudioPolicy = policy);
    final settings = AppSettingsScope.of(context);
    settings.defaultAudioPlaybackPolicy = policy;
    await settings.save();
  }

  Future<void> _setPerformanceAlertPolicy(PerformanceAlertPolicy policy) async {
    if (_performanceAlertPolicy == policy) return;
    setState(() => _performanceAlertPolicy = policy);
    widget.onPerformanceAlertPolicyChanged?.call(policy);
    final settings = AppSettingsScope.of(context);
    settings.performanceAlertPolicy = policy;
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
              fireAndLog(
                'set seek behavior preference',
                _setSeekBehavior(value),
              );
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
              fireAndLog(
                'set viewport pixel size preference',
                _setPixelSizeMode(value),
              );
            },
          ),
          SettingsPageStyle.contentGap,
          SettingsComboRow<DefaultAudioPlaybackPolicy>(
            label: l.defaultAudioPlaybackPolicy,
            icon: Icons.volume_up,
            value: _defaultAudioPolicy,
            items: DefaultAudioPlaybackPolicy.values,
            labelFor: (value) => switch (value) {
              DefaultAudioPlaybackPolicy.muted => l.defaultAudioMuted,
              DefaultAudioPlaybackPolicy.playFirstTrack =>
                l.defaultAudioPlayFirstTrack,
            },
            onChanged: (value) {
              fireAndLog(
                'set default audio preference',
                _setDefaultAudioPolicy(value),
              );
            },
          ),
          SettingsPageStyle.contentGap,
          SettingsComboRow<PerformanceAlertPolicy>(
            label: l.performanceAlertPolicy,
            icon: Icons.speed,
            value: _performanceAlertPolicy,
            items: PerformanceAlertPolicy.values,
            labelFor: (value) => switch (value) {
              PerformanceAlertPolicy.once => l.performanceAlertOnce,
              PerformanceAlertPolicy.sustained => l.performanceAlertSustained,
              PerformanceAlertPolicy.disabled => l.performanceAlertDisabled,
            },
            onChanged: (value) {
              fireAndLog(
                'set performance alert preference',
                _setPerformanceAlertPolicy(value),
              );
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
              fireAndLog('set decode mode preference', _setDecodeMode(value));
            },
          ),
        ],
      ),
    );
  }
}

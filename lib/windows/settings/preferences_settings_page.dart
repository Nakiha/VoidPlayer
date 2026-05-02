import 'dart:async';

import 'package:flutter/material.dart';

import '../../config/app_config.dart';
import '../../l10n/app_localizations.dart';
import '../../preferences/playback_preferences.dart';
import 'settings_page_style.dart';

class PreferencesSettingsPage extends StatefulWidget {
  const PreferencesSettingsPage({super.key});

  @override
  State<PreferencesSettingsPage> createState() =>
      _PreferencesSettingsPageState();
}

class _PreferencesSettingsPageState extends State<PreferencesSettingsPage> {
  late SeekAfterJumpBehavior _seekBehavior;

  @override
  void initState() {
    super.initState();
    _seekBehavior = AppConfig.instance.seekAfterJumpBehavior;
  }

  Future<void> _setSeekBehavior(SeekAfterJumpBehavior behavior) async {
    if (_seekBehavior == behavior) return;
    setState(() => _seekBehavior = behavior);
    AppConfig.instance.seekAfterJumpBehavior = behavior;
    await AppConfig.instance.save();
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
        ],
      ),
    );
  }
}

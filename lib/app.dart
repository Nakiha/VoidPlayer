import 'package:flutter/material.dart';

import 'actions/action_registry.dart';
import 'config/app_config.dart';
import 'config/app_settings_repository.dart';
import 'feedback/app_feedback.dart';
import 'l10n/app_localizations.dart';
import 'preferences/app_config_playback_preferences.dart';
import 'startup_options.dart';
import 'theme/app_appearance.dart';
import 'windows/main/main_window.dart';
import 'windows/window_manager.dart' show AnalysisProcessManager;

class VoidPlayerApp extends StatefulWidget {
  final Color accentColor;
  final AnalysisProcessManager analysisProcesses;
  final String? testScriptPath;
  final StartupOptions startupOptions;

  const VoidPlayerApp({
    super.key,
    required this.accentColor,
    required this.analysisProcesses,
    this.testScriptPath,
    this.startupOptions = const StartupOptions(),
  });

  @override
  State<VoidPlayerApp> createState() => _VoidPlayerAppState();
}

class _VoidPlayerAppState extends State<VoidPlayerApp> {
  static const _fontFamily = 'Segoe UI';
  static const _fontFamilyFallback = [
    'Microsoft YaHei UI',
    'Microsoft YaHei',
    'Microsoft JhengHei UI',
    'Microsoft JhengHei',
    'SimSun',
  ];

  late final ActionRegistry _actionRegistry = ActionRegistry();
  late final AppFeedbackController _feedbackController =
      AppFeedbackController();
  late final AppSettingsRepository _settingsRepository =
      AppConfigSettingsRepository(AppConfig.instance);
  late final AppAppearanceController _appearance;

  @override
  void initState() {
    super.initState();
    _appearance = AppAppearanceController.load(
      settings: _settingsRepository,
      systemAccentColor: widget.accentColor,
    )..addListener(_syncAccentColor);
    _syncAccentColor();
  }

  @override
  void dispose() {
    _appearance.removeListener(_syncAccentColor);
    _appearance.dispose();
    _feedbackController.dispose();
    super.dispose();
  }

  void _syncAccentColor() {
    widget.analysisProcesses.accentColorValue = _appearance.accentColor
        .toARGB32();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: _appearance,
      builder: (context, _) {
        final accentColor = _appearance.accentColor;
        return AppFeedbackScope(
          controller: _feedbackController,
          child: AppSettingsScope(
            settings: _settingsRepository,
            child: AppAppearanceScope(
              controller: _appearance,
              child: MaterialApp(
                title: 'Void Player',
                localizationsDelegates: AppLocalizations.localizationsDelegates,
                supportedLocales: AppLocalizations.supportedLocales,
                themeAnimationDuration: const Duration(milliseconds: 180),
                themeAnimationCurve: Curves.easeOutCubic,
                theme: ThemeData(
                  fontFamily: _fontFamily,
                  fontFamilyFallback: _fontFamilyFallback,
                  colorScheme: ColorScheme.fromSeed(
                    seedColor: accentColor,
                    brightness: Brightness.light,
                  ),
                ),
                darkTheme: ThemeData(
                  fontFamily: _fontFamily,
                  fontFamilyFallback: _fontFamilyFallback,
                  colorScheme: ColorScheme.fromSeed(
                    seedColor: accentColor,
                    brightness: Brightness.dark,
                  ),
                ),
                themeMode: _appearance.themeMode,
                home: ActionFocus(
                  actionRegistry: _actionRegistry,
                  child: MainWindow(
                    actionRegistry: _actionRegistry,
                    testScriptPath: widget.testScriptPath,
                    startupOptions: widget.startupOptions,
                    analysisProcesses: widget.analysisProcesses,
                    appSettings: _settingsRepository,
                    playbackPreferences: AppConfigPlaybackPreferences(
                      _settingsRepository,
                    ),
                  ),
                ),
              ),
            ),
          ),
        );
      },
    );
  }
}

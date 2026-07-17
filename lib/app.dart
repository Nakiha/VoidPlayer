import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import 'actions/action_registry.dart';
import 'config/app_config.dart';
import 'config/app_settings_repository.dart';
import 'feedback/app_feedback.dart';
import 'l10n/app_localizations.dart';
import 'main_window/main_window.dart';
import 'platform/analysis_process_host.dart';
import 'platform/main_window_platform.dart';
import 'platform/native_file_picker.dart';
import 'platform/platform_capabilities.dart';
import 'platform/pointer_button_state_provider.dart';
import 'platform/system_accent_watcher.dart';
import 'preferences/app_config_playback_preferences.dart';
import 'startup_options.dart';
import 'theme/app_appearance.dart';
import 'theme/app_typography.dart';

class VoidPlayerApp extends StatefulWidget {
  final Color accentColor;
  final AnalysisProcessHost analysisProcesses;
  final PlatformCapabilities platformCapabilities;
  final SystemAccentWatcher Function({required ValueChanged<Color> onChanged})
  systemAccentWatcherFactory;
  final MainWindowPlatform platformWindow;
  final NativeFilePicker nativeFilePicker;
  final PointerButtonStateProvider pointerButtonStateProvider;
  final String? testScriptPath;
  final StartupOptions startupOptions;

  const VoidPlayerApp({
    super.key,
    required this.accentColor,
    required this.analysisProcesses,
    required this.platformCapabilities,
    required this.systemAccentWatcherFactory,
    required this.platformWindow,
    required this.nativeFilePicker,
    this.pointerButtonStateProvider = emptyPointerButtonStateProvider,
    this.testScriptPath,
    this.startupOptions = const StartupOptions(),
  });

  @override
  State<VoidPlayerApp> createState() => _VoidPlayerAppState();
}

class _VoidPlayerAppState extends State<VoidPlayerApp> {
  late final ActionRegistry _actionRegistry = ActionRegistry(
    useWindowsRunnerShortcuts: defaultTargetPlatform == TargetPlatform.windows,
  );
  late final AppFeedbackController _feedbackController =
      AppFeedbackController();
  late final AppSettingsRepository _settingsRepository =
      AppConfigSettingsRepository(AppConfig.instance);
  late final AppAppearanceController _appearance;
  late final SystemAccentWatcher _systemAccentWatcher;

  @override
  void initState() {
    super.initState();
    _appearance = AppAppearanceController.load(
      settings: _settingsRepository,
      systemAccentColor: widget.accentColor,
    )..addListener(_syncAccentColor);
    _systemAccentWatcher = widget.systemAccentWatcherFactory(
      onChanged: _appearance.setSystemAccentColor,
    )..start();
    _syncAccentColor();
  }

  @override
  void dispose() {
    _systemAccentWatcher.dispose();
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
        final typography = AppTypography.forPlatform(defaultTargetPlatform);
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
                  fontFamily: typography.fontFamily,
                  fontFamilyFallback: typography.fontFamilyFallback,
                  tooltipTheme: const TooltipThemeData(
                    excludeFromSemantics: true,
                  ),
                  colorScheme: ColorScheme.fromSeed(
                    seedColor: accentColor,
                    brightness: Brightness.light,
                  ),
                ),
                darkTheme: ThemeData(
                  fontFamily: typography.fontFamily,
                  fontFamilyFallback: typography.fontFamilyFallback,
                  tooltipTheme: const TooltipThemeData(
                    excludeFromSemantics: true,
                  ),
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
                    platformCapabilities: widget.platformCapabilities,
                    platformWindow: widget.platformWindow,
                    nativeFilePicker: widget.nativeFilePicker,
                    pointerButtonStateProvider:
                        widget.pointerButtonStateProvider,
                    appSettings: _settingsRepository,
                    accentColor: accentColor,
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

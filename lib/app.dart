import 'package:flutter/material.dart';

import 'actions/action_registry.dart';
import 'l10n/app_localizations.dart';
import 'startup_options.dart';
import 'theme/app_appearance.dart';
import 'windows/main/main_window.dart';
import 'windows/window_manager.dart';

class VoidPlayerApp extends StatefulWidget {
  final Color accentColor;
  final String? testScriptPath;
  final StartupOptions startupOptions;

  const VoidPlayerApp({
    super.key,
    required this.accentColor,
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

  late final AppAppearanceController _appearance;

  @override
  void initState() {
    super.initState();
    _appearance = AppAppearanceController.load(
      systemAccentColor: widget.accentColor,
    )..addListener(_syncAccentColor);
    _syncAccentColor();
  }

  @override
  void dispose() {
    _appearance.removeListener(_syncAccentColor);
    _appearance.dispose();
    super.dispose();
  }

  void _syncAccentColor() {
    WindowManager.accentColorValue = _appearance.accentColor.toARGB32();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: _appearance,
      builder: (context, _) {
        final accentColor = _appearance.accentColor;
        return AppAppearanceScope(
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
              child: MainWindow(
                testScriptPath: widget.testScriptPath,
                startupOptions: widget.startupOptions,
              ),
            ),
          ),
        );
      },
    );
  }
}

import 'package:flutter/material.dart';

import 'actions/action_registry.dart';
import 'l10n/app_localizations.dart';
import 'windows/main_window.dart';

class VoidPlayerApp extends StatelessWidget {
  final Color accentColor;
  final String? testScriptPath;

  const VoidPlayerApp({
    super.key,
    required this.accentColor,
    this.testScriptPath,
  });

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Void Player',
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: accentColor,
          brightness: Brightness.light,
        ),
      ),
      darkTheme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: accentColor,
          brightness: Brightness.dark,
        ),
      ),
      themeMode: ThemeMode.system,
      home: ActionFocus(child: MainWindow(testScriptPath: testScriptPath)),
    );
  }
}

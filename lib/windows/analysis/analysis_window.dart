import 'package:flutter/material.dart';
import '../../l10n/app_localizations.dart';
import 'analysis_ipc_client.dart';
import 'analysis_window_page.dart';
import 'analysis_window_workspace.dart';

// ===========================================================================
// Analysis window app entry for bitstream visualization.
// ===========================================================================

ThemeData _analysisTheme(Color accentColor) {
  return ThemeData(
    brightness: Brightness.dark,
    colorSchemeSeed: accentColor,
    useMaterial3: true,
    tooltipTheme: const TooltipThemeData(excludeFromSemantics: true),
  );
}

Widget _silenceAnalysisSemantics(BuildContext context, Widget? child) {
  return ExcludeSemantics(child: child ?? const SizedBox.shrink());
}

class AnalysisApp extends StatelessWidget {
  final Color accentColor;
  final String hash;
  final String? fileName;
  final String? testScriptPath;

  const AnalysisApp({
    super.key,
    required this.accentColor,
    required this.hash,
    this.fileName,
    this.testScriptPath,
  });

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: fileName != null
          ? 'Void Player - $fileName'
          : 'Void Player - Analysis',
      debugShowCheckedModeBanner: false,
      theme: _analysisTheme(accentColor),
      builder: _silenceAnalysisSemantics,
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: AnalysisPage(hash: hash, testScriptPath: testScriptPath),
    );
  }
}

class AnalysisWorkspaceApp extends StatelessWidget {
  final Color accentColor;
  final List<String> hashes;
  final List<String?> fileNames;
  final String? testScriptPath;
  final AnalysisIpcClient? ipcClient;

  const AnalysisWorkspaceApp({
    super.key,
    required this.accentColor,
    required this.hashes,
    required this.fileNames,
    this.testScriptPath,
    this.ipcClient,
  });

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Void Player - Analysis',
      debugShowCheckedModeBanner: false,
      theme: _analysisTheme(accentColor),
      builder: _silenceAnalysisSemantics,
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: AnalysisWorkspacePage(
        entries: [
          for (var i = 0; i < hashes.length; i++)
            AnalysisWorkspaceEntry(
              hash: hashes[i],
              fileName: i < fileNames.length ? fileNames[i] : null,
            ),
        ],
        testScriptPath: testScriptPath,
        ipcClient: ipcClient,
      ),
    );
  }
}

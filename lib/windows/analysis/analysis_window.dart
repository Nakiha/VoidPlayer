import 'dart:async';
import 'dart:io';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import '../../app_log.dart';
import '../../analysis/analysis_cache.dart';
import '../../analysis/analysis_ffi.dart';
import '../../analysis/nalu_types.dart';
import '../../l10n/app_localizations.dart';
import '../../widgets/segmented_widget.dart';
import 'analysis_ipc.dart';

part 'analysis_window_workspace.dart';
part 'analysis_window_controls.dart';
part 'analysis_window_charts.dart';
part 'analysis_window_nalu.dart';
part 'analysis_window_test_runner.dart';
part 'analysis_window_page.dart';

// ===========================================================================
// Analysis Window — secondary Flutter window for bitstream visualization
// ===========================================================================

const double _analysisHeaderHeight = 40;
const double _analysisHeaderControlHeight = 32;
const double _analysisHeaderGap = 4;
const EdgeInsets _analysisHeaderPadding = EdgeInsets.all(4);

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
      home: _AnalysisWorkspacePage(
        entries: [
          for (var i = 0; i < hashes.length; i++)
            _AnalysisWorkspaceEntry(
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

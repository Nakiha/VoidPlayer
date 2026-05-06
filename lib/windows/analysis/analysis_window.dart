import 'package:flutter/material.dart';

import '../../l10n/app_localizations.dart';
import 'ipc/analysis_ipc_client.dart';
import 'page/analysis_page.dart';
import 'workspace/analysis_workspace_models.dart';
import 'workspace/analysis_workspace_page.dart';

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

class AnalysisWorkspaceApp extends StatefulWidget {
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
  State<AnalysisWorkspaceApp> createState() => _AnalysisWorkspaceAppState();
}

class _AnalysisWorkspaceAppState extends State<AnalysisWorkspaceApp> {
  late Color _accentColor = widget.accentColor;

  @override
  void initState() {
    super.initState();
    widget.ipcClient?.addListener(_onIpcChanged);
    _onIpcChanged();
  }

  @override
  void didUpdateWidget(covariant AnalysisWorkspaceApp oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.ipcClient != widget.ipcClient) {
      oldWidget.ipcClient?.removeListener(_onIpcChanged);
      widget.ipcClient?.addListener(_onIpcChanged);
    }
    if (oldWidget.accentColor != widget.accentColor) {
      _accentColor = widget.accentColor;
    }
    _onIpcChanged();
  }

  @override
  void dispose() {
    widget.ipcClient?.removeListener(_onIpcChanged);
    widget.ipcClient?.dispose();
    super.dispose();
  }

  void _onIpcChanged() {
    final value = widget.ipcClient?.accentColorValue;
    if (value == null) return;
    final color = Color(value);
    if (color == _accentColor) return;
    if (!mounted) {
      _accentColor = color;
      return;
    }
    setState(() => _accentColor = color);
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Void Player - Analysis',
      debugShowCheckedModeBanner: false,
      theme: _analysisTheme(_accentColor),
      builder: _silenceAnalysisSemantics,
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: AnalysisWorkspacePage(
        entries: [
          for (var i = 0; i < widget.hashes.length; i++)
            AnalysisWorkspaceEntry(
              hash: widget.hashes[i],
              fileName: i < widget.fileNames.length
                  ? widget.fileNames[i]
                  : null,
            ),
        ],
        testScriptPath: widget.testScriptPath,
        ipcClient: widget.ipcClient,
      ),
    );
  }
}

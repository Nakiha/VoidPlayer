import 'package:flutter/material.dart';

import '../../../l10n/app_localizations.dart';
import '../ipc/analysis_ipc_client.dart';
import '../page/analysis_page.dart';
import '../widgets/analysis_split_layout_controller.dart';
import 'analysis_workspace_models.dart';
import 'analysis_workspace_split.dart';
import 'analysis_workspace_tabs.dart';

class AnalysisWorkspacePage extends StatefulWidget {
  final List<AnalysisWorkspaceEntry> entries;
  final String? testScriptPath;
  final AnalysisIpcClient? ipcClient;

  const AnalysisWorkspacePage({
    super.key,
    required this.entries,
    this.testScriptPath,
    this.ipcClient,
  });

  @override
  State<AnalysisWorkspacePage> createState() => _AnalysisWorkspacePageState();
}

class _AnalysisWorkspacePageState extends State<AnalysisWorkspacePage> {
  int _selected = 0;
  bool _splitView = false;
  bool _disposed = false;
  late List<AnalysisWorkspaceEntry> _entries;
  final _splitLayout = AnalysisSplitLayoutController();

  int _clampIndex(int value, int length) {
    if (length <= 0) return 0;
    return value.clamp(0, length - 1).toInt();
  }

  @override
  void initState() {
    super.initState();
    _entries = widget.entries;
    widget.ipcClient?.addListener(_onIpcTracksChanged);
    _onIpcTracksChanged();
  }

  @override
  void dispose() {
    _disposed = true;
    widget.ipcClient?.removeListener(_onIpcTracksChanged);
    widget.ipcClient?.dispose();
    _splitLayout.dispose();
    super.dispose();
  }

  void _onIpcTracksChanged() {
    if (_disposed) return;
    final client = widget.ipcClient;
    if (client == null) return;
    if (!client.connected) {
      if (mounted) setState(() {});
      return;
    }
    if (!client.hasSnapshot) return;
    final selectedHash = _entries.isNotEmpty
        ? _entries[_clampIndex(_selected, _entries.length)].hash
        : null;
    final entries = [
      for (final track in client.tracks)
        if (track.hash.isNotEmpty) AnalysisWorkspaceEntry.fromIpcTrack(track),
    ];
    final nextSelected = selectedHash == null
        ? entries.isEmpty
              ? 0
              : _clampIndex(_selected, entries.length)
        : entries.indexWhere((entry) => entry.hash == selectedHash);

    void applySnapshot() {
      _entries = entries;
      if (entries.length <= 1) {
        _splitView = false;
      }
      _selected = entries.isEmpty
          ? 0
          : nextSelected >= 0
          ? nextSelected
          : _clampIndex(_selected, entries.length);
    }

    if (!mounted) {
      applySnapshot();
      return;
    }
    setState(applySnapshot);
  }

  @override
  Widget build(BuildContext context) {
    final entries = _entries;
    if (entries.isEmpty) {
      return const Scaffold(body: SizedBox.shrink());
    }
    final selected = _clampIndex(_selected, entries.length);
    final modeToggleEnabled = entries.length > 1;
    final ipcDisconnected =
        widget.ipcClient != null && !widget.ipcClient!.connected;

    return Scaffold(
      body: Column(
        children: [
          if (ipcDisconnected) const _IpcDisconnectedBanner(),
          Expanded(
            child: _splitView
                ? AnalysisSplitView(
                    entries: entries,
                    splitView: _splitView,
                    modeToggleEnabled: modeToggleEnabled,
                    selectedIndex: selected,
                    layoutController: _splitLayout,
                    onModeChanged: (value) =>
                        setState(() => _splitView = value),
                    onSelected: (index) => setState(() => _selected = index),
                  )
                : AnalysisTabbedView(
                    entries: entries,
                    selectedIndex: selected,
                    splitView: _splitView,
                    modeToggleEnabled: modeToggleEnabled,
                    onModeChanged: (value) =>
                        setState(() => _splitView = value),
                    onSelected: (index) => setState(() => _selected = index),
                    child: AnalysisPage(
                      key: ValueKey('analysis-${entries[selected].hash}'),
                      hash: entries[selected].hash,
                      testScriptPath: selected == 0
                          ? widget.testScriptPath
                          : null,
                      pollSummary: false,
                    ),
                  ),
          ),
        ],
      ),
    );
  }
}

class _IpcDisconnectedBanner extends StatelessWidget {
  const _IpcDisconnectedBanner();

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    return Material(
      color: theme.colorScheme.errorContainer,
      child: SizedBox(
        height: 32,
        child: Row(
          children: [
            const SizedBox(width: 12),
            Icon(
              Icons.link_off,
              size: 16,
              color: theme.colorScheme.onErrorContainer,
            ),
            const SizedBox(width: 8),
            Expanded(
              child: Text(
                l.analysisIpcDisconnected,
                style: theme.textTheme.bodySmall?.copyWith(
                  color: theme.colorScheme.onErrorContainer,
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

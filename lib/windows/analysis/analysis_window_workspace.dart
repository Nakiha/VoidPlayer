import 'package:flutter/material.dart';

import 'analysis_ipc_client.dart';
import 'analysis_split_layout_controller.dart';
import 'analysis_window_page.dart';
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
  late List<AnalysisWorkspaceEntry> _entries;
  final _splitLayout = AnalysisSplitLayoutController();

  @override
  void initState() {
    super.initState();
    _entries = widget.entries;
    widget.ipcClient?.addListener(_onIpcTracksChanged);
    _onIpcTracksChanged();
  }

  @override
  void dispose() {
    widget.ipcClient?.removeListener(_onIpcTracksChanged);
    widget.ipcClient?.dispose();
    _splitLayout.dispose();
    super.dispose();
  }

  void _onIpcTracksChanged() {
    final client = widget.ipcClient;
    if (client == null || !client.hasSnapshot) return;
    final selectedHash = _entries.isNotEmpty
        ? _entries[_selected.clamp(0, _entries.length - 1)].hash
        : null;
    final entries = [
      for (final track in client.tracks)
        if (track.hash.isNotEmpty) AnalysisWorkspaceEntry.fromIpcTrack(track),
    ];
    final nextSelected = selectedHash == null
        ? entries.isEmpty
              ? 0
              : _selected.clamp(0, entries.length - 1)
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
          : _selected.clamp(0, entries.length - 1);
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
    final selected = _selected.clamp(0, entries.length - 1);
    final modeToggleEnabled = entries.length > 1;

    return Scaffold(
      body: _splitView
          ? AnalysisSplitView(
              entries: entries,
              splitView: _splitView,
              modeToggleEnabled: modeToggleEnabled,
              selectedIndex: selected,
              layoutController: _splitLayout,
              onModeChanged: (value) => setState(() => _splitView = value),
              onSelected: (index) => setState(() => _selected = index),
            )
          : AnalysisTabbedView(
              entries: entries,
              selectedIndex: selected,
              splitView: _splitView,
              modeToggleEnabled: modeToggleEnabled,
              onModeChanged: (value) => setState(() => _splitView = value),
              onSelected: (index) => setState(() => _selected = index),
              child: AnalysisPage(
                key: ValueKey('analysis-${entries[selected].hash}'),
                hash: entries[selected].hash,
                testScriptPath: selected == 0 ? widget.testScriptPath : null,
                pollSummary: false,
              ),
            ),
    );
  }
}

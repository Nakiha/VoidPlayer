import 'package:flutter/material.dart';

import '../../l10n/app_localizations.dart';
import '../../widgets/segmented_widget.dart';
import 'analysis_ipc_client.dart';
import 'analysis_ipc_models.dart';
import 'analysis_split_layout_controller.dart';
import 'analysis_window_page.dart';
import 'analysis_window_style.dart';

class AnalysisWorkspaceEntry {
  final String hash;
  final String? fileName;

  const AnalysisWorkspaceEntry({required this.hash, this.fileName});

  factory AnalysisWorkspaceEntry.fromIpcTrack(AnalysisIpcTrack track) =>
      AnalysisWorkspaceEntry(hash: track.hash, fileName: track.fileName);
}

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
          ? _AnalysisSplitView(
              entries: entries,
              splitView: _splitView,
              modeToggleEnabled: modeToggleEnabled,
              selectedIndex: selected,
              layoutController: _splitLayout,
              onModeChanged: (value) => setState(() => _splitView = value),
              onSelected: (index) => setState(() => _selected = index),
            )
          : _AnalysisTabbedView(
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

class _AnalysisTabbedView extends StatelessWidget {
  final List<AnalysisWorkspaceEntry> entries;
  final int selectedIndex;
  final bool splitView;
  final bool modeToggleEnabled;
  final ValueChanged<bool> onModeChanged;
  final ValueChanged<int> onSelected;
  final Widget child;

  const _AnalysisTabbedView({
    required this.entries,
    required this.selectedIndex,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.onModeChanged,
    required this.onSelected,
    required this.child,
  });

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        _AnalysisTabsHeader(
          entries: entries,
          selectedIndex: selectedIndex,
          splitView: splitView,
          modeToggleEnabled: modeToggleEnabled,
          onModeChanged: onModeChanged,
          onSelected: onSelected,
        ),
        Expanded(child: child),
      ],
    );
  }
}

class _AnalysisTabsHeader extends StatelessWidget {
  final List<AnalysisWorkspaceEntry> entries;
  final int selectedIndex;
  final bool splitView;
  final bool modeToggleEnabled;
  final ValueChanged<bool> onModeChanged;
  final ValueChanged<int> onSelected;

  const _AnalysisTabsHeader({
    required this.entries,
    required this.selectedIndex,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.onModeChanged,
    required this.onSelected,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Container(
      height: analysisHeaderHeight,
      padding: analysisHeaderPadding,
      decoration: BoxDecoration(
        color: theme.colorScheme.primaryContainer.withValues(alpha: 0.18),
        border: Border(bottom: BorderSide(color: theme.dividerColor)),
      ),
      child: Row(
        children: [
          _AnalysisWorkspaceModeToggle(
            splitView: splitView,
            enabled: modeToggleEnabled,
            onChanged: onModeChanged,
          ),
          const SizedBox(width: analysisHeaderGap),
          Expanded(
            child: Row(
              children: [
                for (var i = 0; i < entries.length; i++)
                  Expanded(
                    child: _AnalysisTrackTab(
                      entry: entries[i],
                      index: i,
                      selected: i == selectedIndex,
                      onTap: () => onSelected(i),
                    ),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _AnalysisTrackTab extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;
  final int index;
  final bool selected;
  final VoidCallback onTap;

  const _AnalysisTrackTab({
    required this.entry,
    required this.index,
    required this.selected,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 2),
      child: _AnalysisTrackTitleButton(
        entry: entry,
        index: index,
        selected: selected,
        onTap: selected ? null : onTap,
      ),
    );
  }
}

class _AnalysisTrackTitleButton extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;
  final int index;
  final bool selected;
  final VoidCallback? onTap;

  const _AnalysisTrackTitleButton({
    required this.entry,
    required this.index,
    required this.selected,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return SizedBox(
      height: analysisHeaderControlHeight,
      child: Material(
        color: selected
            ? theme.colorScheme.primary.withValues(alpha: 0.10)
            : Colors.transparent,
        borderRadius: BorderRadius.circular(4),
        clipBehavior: Clip.antiAlias,
        child: InkWell(
          borderRadius: BorderRadius.circular(4),
          onTap: onTap,
          child: Stack(
            children: [
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 10),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: Text(
                    '${index + 1}. ${entry.fileName ?? 'Track ${index + 1}'}',
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    softWrap: false,
                    style: theme.textTheme.titleSmall?.copyWith(
                      color: selected
                          ? theme.colorScheme.onSurface
                          : theme.colorScheme.onSurfaceVariant,
                      fontWeight: selected ? FontWeight.w600 : null,
                    ),
                  ),
                ),
              ),
              if (selected)
                Positioned(
                  left: 10,
                  right: 10,
                  bottom: 2,
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      color: theme.colorScheme.primary.withValues(alpha: 0.52),
                      borderRadius: BorderRadius.circular(1),
                    ),
                    child: const SizedBox(height: 2),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class _AnalysisSplitView extends StatelessWidget {
  final List<AnalysisWorkspaceEntry> entries;
  final bool splitView;
  final bool modeToggleEnabled;
  final int selectedIndex;
  final AnalysisSplitLayoutController layoutController;
  final ValueChanged<bool> onModeChanged;
  final ValueChanged<int> onSelected;

  const _AnalysisSplitView({
    required this.entries,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.selectedIndex,
    required this.layoutController,
    required this.onModeChanged,
    required this.onSelected,
  });

  @override
  Widget build(BuildContext context) {
    final count = entries.length;
    final columns = count <= 2 ? count : 2;
    final rows = (count / columns).ceil();

    return Column(
      children: [
        for (var row = 0; row < rows; row++)
          Expanded(
            child: Row(
              children: [
                for (var col = 0; col < columns; col++)
                  Expanded(
                    child: _splitCell(
                      context,
                      row * columns + col,
                      row: row,
                      col: col,
                      rows: rows,
                      columns: columns,
                    ),
                  ),
              ],
            ),
          ),
      ],
    );
  }

  Widget _splitCell(
    BuildContext context,
    int index, {
    required int row,
    required int col,
    required int rows,
    required int columns,
  }) {
    if (index >= entries.length) return const SizedBox.shrink();
    final entry = entries[index];
    final theme = Theme.of(context);
    final divider = BorderSide(color: theme.colorScheme.outlineVariant);
    return Container(
      decoration: BoxDecoration(
        border: Border(
          right: col < columns - 1 ? divider : BorderSide.none,
          bottom: row < rows - 1 ? divider : BorderSide.none,
        ),
      ),
      child: _AnalysisTrackPane(
        entry: entry,
        index: index,
        selected: index == selectedIndex,
        showModeToggle: index == 0,
        splitView: splitView,
        modeToggleEnabled: modeToggleEnabled,
        onModeChanged: onModeChanged,
        onSelected: () => onSelected(index),
        child: AnalysisPage(
          key: ValueKey('analysis-split-${entry.hash}'),
          hash: entry.hash,
          pollSummary: false,
          splitLayoutController: layoutController,
        ),
      ),
    );
  }
}

class _AnalysisTrackPane extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;
  final int index;
  final bool selected;
  final bool showModeToggle;
  final bool splitView;
  final bool modeToggleEnabled;
  final ValueChanged<bool> onModeChanged;
  final VoidCallback? onSelected;
  final Widget child;

  const _AnalysisTrackPane({
    required this.entry,
    required this.index,
    required this.selected,
    required this.showModeToggle,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.onModeChanged,
    required this.child,
    this.onSelected,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Column(
      children: [
        Container(
          height: analysisHeaderHeight,
          padding: analysisHeaderPadding,
          decoration: BoxDecoration(
            color: selected
                ? theme.colorScheme.primaryContainer.withValues(alpha: 0.18)
                : theme.colorScheme.surface.withValues(alpha: 0.18),
            border: Border(bottom: BorderSide(color: theme.dividerColor)),
          ),
          child: Row(
            children: [
              if (showModeToggle) ...[
                _AnalysisWorkspaceModeToggle(
                  splitView: splitView,
                  enabled: modeToggleEnabled,
                  onChanged: onModeChanged,
                ),
                const SizedBox(width: analysisHeaderGap),
              ],
              Expanded(
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 2),
                  child: _AnalysisTrackTitleButton(
                    entry: entry,
                    index: index,
                    selected: selected,
                    onTap: onSelected,
                  ),
                ),
              ),
            ],
          ),
        ),
        Expanded(child: child),
      ],
    );
  }
}

class _AnalysisWorkspaceModeToggle extends StatelessWidget {
  final bool splitView;
  final bool enabled;
  final ValueChanged<bool> onChanged;

  const _AnalysisWorkspaceModeToggle({
    required this.splitView,
    required this.enabled,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return ExcludeSemantics(
      child: ViewModeSelector(
        currentMode: splitView ? 1 : 0,
        onChanged: (value) => onChanged(value == 1),
        firstLabel: l.analysisTabsMode,
        secondLabel: l.analysisSplitMode,
        width: 124,
        height: analysisHeaderControlHeight,
        enabled: enabled,
        labelFontWeight: FontWeight.w700,
      ),
    );
  }
}

// ===========================================================================

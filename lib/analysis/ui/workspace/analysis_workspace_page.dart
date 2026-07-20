import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../../../l10n/app_localizations.dart';
import '../../analysis_manager.dart';
import '../analysis_ui_selection.dart';
import '../page/analysis_page.dart';
import '../page/analysis_page_state.dart';
import '../testing/analysis_test_host.dart';
import '../widgets/analysis_split_layout_controller.dart';
import 'analysis_workspace_models.dart';
import 'analysis_workspace_split.dart';
import 'analysis_workspace_tabs.dart';

class AnalysisWorkspacePage extends StatefulWidget {
  final ValueListenable<List<AnalysisWorkspaceEntry>> entries;
  final AnalysisTestHostRegistry testHosts;
  final ValueChanged<AnalysisUiSelection?>? onSelectionChanged;
  final Map<int, AnalysisPlaybackPosition> currentPlaybackByFileId;
  final ValueChanged<AnalysisFrameSeekRequest>? onFrameSeekRequested;
  final AnalysisWorkspaceContentBuilder? contentBuilder;
  final AnalysisWorkspaceFallbackBuilder? fallbackBuilder;

  const AnalysisWorkspacePage({
    super.key,
    required this.entries,
    required this.testHosts,
    this.onSelectionChanged,
    this.currentPlaybackByFileId = const {},
    this.onFrameSeekRequested,
    this.contentBuilder,
    this.fallbackBuilder,
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
    _entries = widget.entries.value;
    widget.testHosts.selectFileId(
      _entries.isEmpty ? null : _entries.first.fileId,
    );
    widget.entries.addListener(_onEntriesChanged);
  }

  @override
  void dispose() {
    _disposed = true;
    widget.entries.removeListener(_onEntriesChanged);
    _splitLayout.dispose();
    super.dispose();
  }

  @override
  void didUpdateWidget(covariant AnalysisWorkspacePage oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.entries == widget.entries) return;
    oldWidget.entries.removeListener(_onEntriesChanged);
    widget.entries.addListener(_onEntriesChanged);
    _onEntriesChanged();
  }

  void _onEntriesChanged() {
    if (_disposed) return;
    final selectedFileId = _entries.isNotEmpty
        ? _entries[_clampIndex(_selected, _entries.length)].fileId
        : null;
    final entries = widget.entries.value;
    final nextSelected = selectedFileId == null
        ? entries.isEmpty
              ? 0
              : _clampIndex(_selected, entries.length)
        : entries.indexWhere((entry) => entry.fileId == selectedFileId);
    if (selectedFileId != null && nextSelected < 0) {
      widget.onSelectionChanged?.call(null);
    }

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
      widget.testHosts.selectFileId(
        entries.isEmpty ? null : entries[_selected].fileId,
      );
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
      final fallbackBuilder = widget.fallbackBuilder;
      if (fallbackBuilder != null) {
        return fallbackBuilder(context, entries, 0, _selectEntry, null);
      }
      return const Scaffold(body: SizedBox.shrink());
    }
    final selected = _clampIndex(_selected, entries.length);
    final modeToggleEnabled = entries.length > 1;
    if (widget.contentBuilder != null) {
      return IndexedStack(
        index: selected,
        children: [
          for (var index = 0; index < entries.length; index++)
            _buildEntry(
              entries[index],
              dockActive: index == selected,
              dockSelectedIndex: selected,
            ),
        ],
      );
    }

    return Scaffold(
      body: _splitView
          ? AnalysisSplitView(
              entries: entries,
              splitView: _splitView,
              modeToggleEnabled: modeToggleEnabled,
              selectedIndex: selected,
              layoutController: _splitLayout,
              onModeChanged: (value) => setState(() => _splitView = value),
              onSelected: (index) {
                widget.testHosts.selectFileId(entries[index].fileId);
                widget.onSelectionChanged?.call(null);
                setState(() => _selected = index);
              },
              contentBuilder: (entry) => _buildEntry(entry, split: true),
            )
          : AnalysisTabbedView(
              entries: entries,
              selectedIndex: selected,
              splitView: _splitView,
              modeToggleEnabled: modeToggleEnabled,
              onModeChanged: (value) => setState(() => _splitView = value),
              onSelected: (index) {
                widget.testHosts.selectFileId(entries[index].fileId);
                widget.onSelectionChanged?.call(null);
                setState(() => _selected = index);
              },
              child: _buildEntry(entries[selected]),
            ),
    );
  }

  Widget _buildEntry(
    AnalysisWorkspaceEntry entry, {
    bool split = false,
    bool dockActive = true,
    int? dockSelectedIndex,
  }) {
    final hash = entry.hash;
    if (hash != null && hash.isNotEmpty) {
      return AnalysisPage(
        key: ValueKey('analysis-${split ? 'split-' : ''}${entry.fileId}-$hash'),
        fileId: entry.fileId,
        hash: hash,
        testHosts: widget.testHosts,
        pollSummary: false,
        splitLayoutController: split ? _splitLayout : null,
        onSelectionChanged: widget.onSelectionChanged,
        currentPlaybackPosition: widget.currentPlaybackByFileId[entry.fileId],
        onFrameSeekRequested: widget.onFrameSeekRequested,
        contentBuilder: widget.contentBuilder == null
            ? null
            : dockActive
            ? (context, model, actions) => widget.contentBuilder!(
                context,
                entry,
                _entries,
                dockSelectedIndex ?? _selected,
                _selectEntry,
                model,
                actions,
              )
            : (context, model, actions) => const SizedBox.shrink(),
      );
    }
    if (!dockActive) return const SizedBox.shrink();
    final fallbackBuilder = widget.fallbackBuilder;
    if (fallbackBuilder != null) {
      return fallbackBuilder(context, _entries, _selected, _selectEntry, entry);
    }
    return _AnalysisGenerationPlaceholder(entry: entry);
  }

  void _selectEntry(int index) {
    if (index < 0 || index >= _entries.length || index == _selected) return;
    widget.testHosts.selectFileId(_entries[index].fileId);
    widget.onSelectionChanged?.call(null);
    setState(() => _selected = index);
  }
}

typedef AnalysisWorkspaceContentBuilder =
    Widget Function(
      BuildContext context,
      AnalysisWorkspaceEntry entry,
      List<AnalysisWorkspaceEntry> entries,
      int selectedIndex,
      ValueChanged<int> onSelected,
      AnalysisPageViewModel model,
      AnalysisPageActions actions,
    );

typedef AnalysisWorkspaceFallbackBuilder =
    Widget Function(
      BuildContext context,
      List<AnalysisWorkspaceEntry> entries,
      int selectedIndex,
      ValueChanged<int> onSelected,
      AnalysisWorkspaceEntry? entry,
    );

class _AnalysisGenerationPlaceholder extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;

  const _AnalysisGenerationPlaceholder({required this.entry});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    final status = entry.generationStatus;
    final failed = status?.isError ?? false;
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            if (failed)
              Icon(Icons.error_outline, color: theme.colorScheme.error)
            else
              SizedBox(
                width: 28,
                height: 28,
                child: CircularProgressIndicator(
                  value: status?.status == AnalysisTrackStatus.generating
                      ? status!.progress.clamp(0.0, 1.0)
                      : null,
                ),
              ),
            const SizedBox(height: 12),
            Text(entry.fileName, maxLines: 1, overflow: TextOverflow.ellipsis),
            const SizedBox(height: 4),
            Text(
              _statusText(l, status),
              style: theme.textTheme.bodySmall?.copyWith(
                color: failed
                    ? theme.colorScheme.error
                    : theme.colorScheme.onSurfaceVariant,
              ),
            ),
          ],
        ),
      ),
    );
  }

  String _statusText(
    AppLocalizations l,
    AnalysisTrackGenerationStatus? status,
  ) {
    if (status?.isError ?? false) return l.analysisCacheStatusFailed;
    return switch (status?.status) {
      AnalysisTrackStatus.generating => l.analysisGeneratingFor(
        status!.fileName,
      ),
      AnalysisTrackStatus.loading => l.analysisCacheStatusLoading,
      AnalysisTrackStatus.cached => l.analysisCacheStatusCached,
      AnalysisTrackStatus.idle => l.analysisCacheStatusMissing,
      AnalysisTrackStatus.computingHash ||
      null => l.analysisCacheStatusChecking,
      AnalysisTrackStatus.error => l.analysisCacheStatusFailed,
    };
  }
}

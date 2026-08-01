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

const Key analysisWorkspaceSplitDividerKey = ValueKey(
  'main-window-analysis-split-divider',
);
const Key analysisWorkspaceFocusedSplitCellKey = ValueKey(
  'main-window-analysis-focused-split-cell',
);

Key analysisWorkspaceSplitCellKey(int fileId) =>
    ValueKey('main-window-analysis-split-cell-$fileId');

class AnalysisWorkspacePage extends StatefulWidget {
  final ValueListenable<List<AnalysisWorkspaceEntry>> entries;
  final AnalysisTestHostRegistry testHosts;
  final ValueChanged<AnalysisUiSelection?>? onSelectionChanged;
  final Map<int, AnalysisPlaybackPosition> currentPlaybackByFileId;
  final ValueChanged<AnalysisFrameSeekRequest>? onFrameSeekRequested;
  final AnalysisWorkspaceContentBuilder? contentBuilder;
  final AnalysisWorkspaceFallbackBuilder? fallbackBuilder;
  final bool splitView;

  const AnalysisWorkspacePage({
    super.key,
    required this.entries,
    required this.testHosts,
    this.onSelectionChanged,
    this.currentPlaybackByFileId = const {},
    this.onFrameSeekRequested,
    this.contentBuilder,
    this.fallbackBuilder,
    this.splitView = false,
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
  List<int> _splitPairFileIds = const [];
  double _deckSplitFraction = 0.5;

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
      return _buildDockWorkspace(
        entries: entries,
        selected: selected,
        splitView: widget.splitView && modeToggleEnabled,
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
    bool dockVisible = true,
    bool dockFocused = true,
    bool splitLayoutPrimary = false,
    int? dockSelectedIndex,
  }) {
    final hash = entry.hash;
    if (hash != null && hash.isNotEmpty) {
      return AnalysisPage(
        key: ValueKey('analysis-${entry.fileId}-$hash'),
        fileId: entry.fileId,
        hash: hash,
        testHosts: widget.testHosts,
        pollSummary: false,
        splitLayoutController: split ? _splitLayout : null,
        splitLayoutPrimary: splitLayoutPrimary,
        onSelectionChanged: dockFocused ? widget.onSelectionChanged : null,
        currentPlaybackPosition: widget.currentPlaybackByFileId[entry.fileId],
        onFrameSeekRequested: widget.onFrameSeekRequested,
        contentBuilder: widget.contentBuilder == null
            ? null
            : dockVisible
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
    if (!dockVisible) return const SizedBox.shrink();
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

  Widget _buildDockWorkspace({
    required List<AnalysisWorkspaceEntry> entries,
    required int selected,
    required bool splitView,
  }) {
    final visibleIndices = splitView
        ? _resolveSplitPair(entries, selected)
        : <int>[selected];
    final firstFileId = entries[visibleIndices.first].fileId;
    final secondFileId = splitView ? entries[visibleIndices.last].fileId : null;
    return Stack(
      children: [
        Positioned.fill(
          child: CustomMultiChildLayout(
            delegate: _AnalysisDeckLayoutDelegate(
              fileIds: [for (final entry in entries) entry.fileId],
              firstFileId: firstFileId,
              secondFileId: secondFileId,
              splitFraction: _deckSplitFraction,
            ),
            children: [
              for (var index = 0; index < entries.length; index++)
                LayoutId(
                  id: entries[index].fileId,
                  child: _AnalysisDeckCell(
                    entry: entries[index],
                    visible: visibleIndices.contains(index),
                    focused: index == selected,
                    splitView: splitView,
                    onFocused: () => _selectEntry(index),
                    child: _buildEntry(
                      entries[index],
                      split: splitView,
                      dockVisible: visibleIndices.contains(index),
                      dockFocused: index == selected,
                      splitLayoutPrimary: splitView && index == selected,
                      dockSelectedIndex: selected,
                    ),
                  ),
                ),
            ],
          ),
        ),
        if (splitView)
          _AnalysisDeckSplitDivider(
            fraction: _deckSplitFraction,
            onChanged: (value) {
              setState(() => _deckSplitFraction = value);
            },
          ),
      ],
    );
  }

  List<int> _resolveSplitPair(
    List<AnalysisWorkspaceEntry> entries,
    int selected,
  ) {
    final indices = [
      for (final fileId in _splitPairFileIds)
        entries.indexWhere((entry) => entry.fileId == fileId),
    ]..removeWhere((index) => index < 0);
    if (indices.length == 2 && indices.contains(selected)) return indices;
    final next = (selected + 1) % entries.length;
    _splitPairFileIds = [entries[selected].fileId, entries[next].fileId];
    return [selected, next];
  }
}

const double _analysisDeckSplitDividerWidth = 8.0;

class _AnalysisDeckLayoutDelegate extends MultiChildLayoutDelegate {
  final List<int> fileIds;
  final int firstFileId;
  final int? secondFileId;
  final double splitFraction;

  _AnalysisDeckLayoutDelegate({
    required this.fileIds,
    required this.firstFileId,
    required this.secondFileId,
    required this.splitFraction,
  });

  @override
  void performLayout(Size size) {
    final second = secondFileId;
    final split = second != null;
    final contentWidth = split
        ? (size.width - _analysisDeckSplitDividerWidth).clamp(
            0.0,
            double.infinity,
          )
        : size.width;
    final firstWidth = split ? contentWidth * splitFraction : contentWidth;
    final secondWidth = contentWidth - firstWidth;
    for (final fileId in fileIds) {
      if (!hasChild(fileId)) continue;
      if (fileId == firstFileId) {
        layoutChild(
          fileId,
          BoxConstraints.tight(Size(firstWidth, size.height)),
        );
        positionChild(fileId, Offset.zero);
      } else if (fileId == second) {
        layoutChild(
          fileId,
          BoxConstraints.tight(Size(secondWidth, size.height)),
        );
        positionChild(
          fileId,
          Offset(firstWidth + _analysisDeckSplitDividerWidth, 0),
        );
      } else {
        layoutChild(fileId, BoxConstraints.tight(Size.zero));
        positionChild(fileId, Offset(-size.width, -size.height));
      }
    }
  }

  @override
  bool shouldRelayout(covariant _AnalysisDeckLayoutDelegate oldDelegate) {
    return !listEquals(fileIds, oldDelegate.fileIds) ||
        firstFileId != oldDelegate.firstFileId ||
        secondFileId != oldDelegate.secondFileId ||
        splitFraction != oldDelegate.splitFraction;
  }
}

class _AnalysisDeckCell extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;
  final bool visible;
  final bool focused;
  final bool splitView;
  final VoidCallback onFocused;
  final Widget child;

  const _AnalysisDeckCell({
    required this.entry,
    required this.visible,
    required this.focused,
    required this.splitView,
    required this.onFocused,
    required this.child,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    Widget result = Semantics(
      selected: splitView && focused,
      label: entry.fileName,
      child: Listener(
        onPointerDown: splitView && visible ? (_) => onFocused() : null,
        child: ColoredBox(
          key: splitView && visible
              ? analysisWorkspaceSplitCellKey(entry.fileId)
              : null,
          color: splitView
              ? focused
                    ? theme.colorScheme.surfaceContainerLowest
                    : theme.colorScheme.surfaceContainerLow
              : theme.colorScheme.surface,
          child: Column(
            children: [
              if (splitView)
                SizedBox(
                  height: 28,
                  child: ColoredBox(
                    color: focused
                        ? theme.colorScheme.primaryContainer
                        : theme.colorScheme.surfaceContainerLow,
                    child: Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 8),
                      child: Align(
                        alignment: Alignment.centerLeft,
                        child: Text(
                          entry.fileName,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: theme.textTheme.labelMedium?.copyWith(
                            color: focused
                                ? theme.colorScheme.onPrimaryContainer
                                : theme.colorScheme.onSurfaceVariant,
                            fontWeight: focused ? FontWeight.w700 : null,
                          ),
                        ),
                      ),
                    ),
                  ),
                )
              else
                const SizedBox.shrink(),
              Expanded(child: child),
            ],
          ),
        ),
      ),
    );
    if (splitView && visible && focused) {
      result = KeyedSubtree(
        key: analysisWorkspaceFocusedSplitCellKey,
        child: result,
      );
    }
    return result;
  }
}

class _AnalysisDeckSplitDivider extends StatelessWidget {
  final double fraction;
  final ValueChanged<double> onChanged;

  const _AnalysisDeckSplitDivider({
    required this.fraction,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final colors = Theme.of(context).colorScheme;
    return Positioned.fill(
      child: LayoutBuilder(
        builder: (context, constraints) {
          final contentWidth =
              constraints.maxWidth - _analysisDeckSplitDividerWidth;
          final left = contentWidth * fraction;
          return Stack(
            children: [
              Positioned(
                key: analysisWorkspaceSplitDividerKey,
                left: left,
                top: 0,
                bottom: 0,
                width: _analysisDeckSplitDividerWidth,
                child: MouseRegion(
                  cursor: SystemMouseCursors.resizeColumn,
                  child: GestureDetector(
                    behavior: HitTestBehavior.opaque,
                    onHorizontalDragUpdate: (details) {
                      if (contentWidth <= 0) return;
                      onChanged(
                        (fraction + details.delta.dx / contentWidth)
                            .clamp(0.25, 0.75)
                            .toDouble(),
                      );
                    },
                    child: Center(
                      child: VerticalDivider(
                        width: 1,
                        color: colors.outlineVariant,
                      ),
                    ),
                  ),
                ),
              ),
            ],
          );
        },
      ),
    );
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

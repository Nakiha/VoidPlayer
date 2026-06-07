import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../analysis/analysis_cache.dart';
import '../../feedback/app_feedback.dart';
import '../../l10n/app_localizations.dart';
import '../../platform/path_launcher.dart';
import '../../theme/app_appearance.dart';
import 'settings_page_style.dart';

const _cacheListTrailingPadding = 12.0;
const _cacheListScrollbarThickness = 6.0;
const _cachePinnedHeaderLoadingExtent = 60.0;
const _cachePinnedHeaderCompactExtent = 80.0;
const _cachePinnedHeaderLimitExtent = 118.0;
const _cacheTopControlsBottomGap = 8.0;
const _cachePinnedHeaderTopPadding = 6.0;
const _cachePinnedHeaderBottomPadding = 6.0;
const _cachePinnedHeaderSectionGap = 10.0;

class CacheSettingsPage extends StatefulWidget {
  final PathLauncher pathLauncher;

  const CacheSettingsPage({
    super.key,
    this.pathLauncher = const LocalPathLauncher(),
  });

  @override
  State<CacheSettingsPage> createState() => _CacheSettingsPageState();
}

class _CacheSettingsPageState extends State<CacheSettingsPage> {
  late final TextEditingController _limitController;
  late final FocusNode _limitFocusNode;
  Timer? _refreshTimer;
  AnalysisCacheSnapshot? _snapshot;
  final Set<String> _selectedHashes = <String>{};
  bool _loading = false;
  bool _deleting = false;

  @override
  void initState() {
    super.initState();
    _limitController = TextEditingController(
      text: _limitInputFromBytes(
        AppSettingsScope.read(context).analysisCacheMaxBytes,
      ),
    );
    _limitFocusNode = FocusNode()..addListener(_onLimitFocusChanged);
    _refresh();
    _refreshTimer = Timer.periodic(
      const Duration(seconds: 5),
      (_) => _refresh(),
    );
  }

  @override
  void dispose() {
    _refreshTimer?.cancel();
    if (_limitFocusNode.hasFocus) {
      unawaited(_saveLimit(refresh: false));
    }
    _limitFocusNode.removeListener(_onLimitFocusChanged);
    _limitFocusNode.dispose();
    _limitController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final snapshot = _snapshot;

    return ScrollbarTheme(
      data: ScrollbarTheme.of(context).copyWith(
        thickness: const WidgetStatePropertyAll(_cacheListScrollbarThickness),
      ),
      child: Scrollbar(
        child: CustomScrollView(
          slivers: [
            SliverPadding(
              padding: const EdgeInsets.fromLTRB(16, 14, 16, 0),
              sliver: SliverToBoxAdapter(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    SettingsPageTitle(
                      text: l.cache,
                      trailing: IconButton(
                        onPressed: _loading ? null : _refresh,
                        icon: const Icon(Icons.refresh, size: 18),
                        tooltip: l.refresh,
                      ),
                    ),
                    const SizedBox(height: 4),
                    if (snapshot != null)
                      _CachePathRow(
                        path: snapshot.path,
                        pathLauncher: widget.pathLauncher,
                      ),
                    SettingsPageStyle.contentGap,
                    _LimitEditor(
                      controller: _limitController,
                      focusNode: _limitFocusNode,
                      onSubmitted: () => _saveLimit(),
                    ),
                    const SizedBox(height: _cacheTopControlsBottomGap),
                  ],
                ),
              ),
            ),
            SliverPersistentHeader(
              pinned: true,
              delegate: _CachePinnedHeaderDelegate(
                snapshot: snapshot,
                selectedCount: _selectedHashes.length,
                selectableCount: snapshot?.entries.length ?? 0,
                deleting: _deleting,
                onSelectAll: _selectAllEntries,
                onCancelSelection: _clearSelection,
                onDeleteSelected: _deleteSelected,
              ),
            ),
            SliverPadding(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
              sliver: _CacheEntriesSliver(
                snapshot: snapshot,
                selectedHashes: _selectedHashes,
                onSelectedChanged: _setSelected,
              ),
            ),
          ],
        ),
      ),
    );
  }

  Future<void> _refresh() async {
    if (_loading) return;
    _loading = true;
    try {
      final snapshot = await AnalysisCache.snapshot(
        maxBytes: AppSettingsScope.read(context).analysisCacheMaxBytes,
      );
      if (!mounted) return;
      setState(() {
        _snapshot = snapshot;
        final liveHashes = snapshot.entries.map((e) => e.hash).toSet();
        _selectedHashes.removeWhere((hash) => !liveHashes.contains(hash));
      });
    } finally {
      _loading = false;
    }
  }

  void _onLimitFocusChanged() {
    if (!_limitFocusNode.hasFocus) {
      unawaited(_saveLimit());
    }
  }

  Future<void> _saveLimit({bool refresh = true}) async {
    final valueMb = int.tryParse(_limitController.text.trim()) ?? 0;
    if (_limitController.text != '$valueMb') {
      _limitController.text = '$valueMb';
    }
    final bytes = valueMb <= 0 ? 0 : valueMb * 1024 * 1024;
    final settings = AppSettingsScope.read(context);
    settings.analysisCacheMaxBytes = bytes;
    await settings.save();
    if (refresh && mounted) await _refresh();
  }

  static String _limitInputFromBytes(int bytes) {
    if (bytes <= 0) return '0';
    return (bytes / (1024 * 1024)).toStringAsFixed(0);
  }

  void _setSelected(String hash, bool selected) {
    setState(() {
      if (selected) {
        _selectedHashes.add(hash);
      } else {
        _selectedHashes.remove(hash);
      }
    });
  }

  void _clearSelection() {
    setState(_selectedHashes.clear);
  }

  void _selectAllEntries() {
    final snapshot = _snapshot;
    if (snapshot == null || snapshot.entries.isEmpty) return;
    setState(() {
      _selectedHashes
        ..clear()
        ..addAll(snapshot.entries.map((entry) => entry.hash));
    });
  }

  Future<void> _deleteSelected() async {
    if (_selectedHashes.isEmpty || _deleting) return;
    final confirmed = await _confirmDeleteSelected();
    if (!confirmed || !mounted) return;

    final hashes = _selectedHashes.toSet();
    setState(() => _deleting = true);
    AnalysisCacheDeleteResult? result;
    Object? error;
    try {
      result = await AnalysisCache.deleteEntries(hashes);
      if (!mounted) return;
      _selectedHashes.removeAll(result.deletedHashes);
      await _refresh();
    } catch (e) {
      error = e;
    } finally {
      if (mounted) {
        setState(() => _deleting = false);
      }
    }
    if (!mounted) return;

    final l = AppLocalizations.of(context)!;
    final deleteResult = result;
    final message = error != null
        ? l.cacheDeleteFailed(hashes.length)
        : deleteResult!.hasFailures
        ? l.cacheDeleteFailed(deleteResult.failedCount)
        : l.cacheDeleted(deleteResult.deletedCount);
    final failed = error != null || (deleteResult?.hasFailures ?? true);
    final feedback = AppFeedbackScope.read(context);
    if (failed) {
      feedback.showError(message);
    } else {
      feedback.show(
        AppFeedbackMessage(
          text: message,
          severity: AppFeedbackSeverity.success,
        ),
      );
    }
  }

  Future<bool> _confirmDeleteSelected() async {
    final l = AppLocalizations.of(context)!;
    final count = _selectedHashes.length;
    final result = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(l.deleteSelectedCache),
        content: Text(l.cacheDeleteConfirmMessage(count)),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: Text(l.cancel),
          ),
          FilledButton(
            onPressed: () => Navigator.of(context).pop(true),
            child: Text(l.delete),
          ),
        ],
      ),
    );
    return result ?? false;
  }
}

class _CachePinnedHeaderDelegate extends SliverPersistentHeaderDelegate {
  final AnalysisCacheSnapshot? snapshot;
  final int selectedCount;
  final int selectableCount;
  final bool deleting;
  final VoidCallback onSelectAll;
  final VoidCallback onCancelSelection;
  final VoidCallback onDeleteSelected;

  const _CachePinnedHeaderDelegate({
    required this.snapshot,
    required this.selectedCount,
    required this.selectableCount,
    required this.deleting,
    required this.onSelectAll,
    required this.onCancelSelection,
    required this.onDeleteSelected,
  });

  double get _extent {
    final snapshot = this.snapshot;
    if (snapshot == null) return _cachePinnedHeaderLoadingExtent;
    if (!snapshot.hasLimit) return _cachePinnedHeaderCompactExtent;
    return _cachePinnedHeaderLimitExtent;
  }

  @override
  double get minExtent => _extent;

  @override
  double get maxExtent => _extent;

  @override
  Widget build(
    BuildContext context,
    double shrinkOffset,
    bool overlapsContent,
  ) {
    final theme = Theme.of(context);
    return Material(
      color: theme.colorScheme.surface,
      elevation: overlapsContent ? 2 : 0,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(
          16,
          _cachePinnedHeaderTopPadding,
          16,
          _cachePinnedHeaderBottomPadding,
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            if (snapshot == null)
              const LinearProgressIndicator()
            else
              _UsageSummary(snapshot: snapshot!),
            const SizedBox(height: _cachePinnedHeaderSectionGap),
            _CacheListHeader(
              selectedCount: selectedCount,
              selectableCount: selectableCount,
              deleting: deleting,
              onSelectAll: onSelectAll,
              onCancelSelection: onCancelSelection,
              onDeleteSelected: onDeleteSelected,
            ),
          ],
        ),
      ),
    );
  }

  @override
  bool shouldRebuild(covariant _CachePinnedHeaderDelegate oldDelegate) {
    return snapshot != oldDelegate.snapshot ||
        selectedCount != oldDelegate.selectedCount ||
        selectableCount != oldDelegate.selectableCount ||
        deleting != oldDelegate.deleting ||
        onSelectAll != oldDelegate.onSelectAll ||
        onCancelSelection != oldDelegate.onCancelSelection ||
        onDeleteSelected != oldDelegate.onDeleteSelected;
  }
}

class _CacheEntriesSliver extends StatelessWidget {
  final AnalysisCacheSnapshot? snapshot;
  final Set<String> selectedHashes;
  final void Function(String hash, bool selected) onSelectedChanged;

  const _CacheEntriesSliver({
    required this.snapshot,
    required this.selectedHashes,
    required this.onSelectedChanged,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final snapshot = this.snapshot;

    if (snapshot == null || snapshot.entries.isEmpty) {
      return SliverFillRemaining(
        hasScrollBody: false,
        child: Center(child: Text(l.cacheNoEntries)),
      );
    }

    final entries = snapshot.entries;
    final hasUnindexed = snapshot.unindexedBytes > 0;
    final tileCount = entries.length + (hasUnindexed ? 1 : 0);
    return SliverList(
      delegate: SliverChildBuilderDelegate((context, index) {
        if (index.isOdd) return const Divider(height: 1);
        final tileIndex = index ~/ 2;
        if (tileIndex >= entries.length) {
          return _UnindexedCacheTile(bytes: snapshot.unindexedBytes);
        }
        final entry = entries[tileIndex];
        return _CacheEntryTile(
          entry: entry,
          selected: selectedHashes.contains(entry.hash),
          onSelectedChanged: (selected) {
            onSelectedChanged(entry.hash, selected);
          },
        );
      }, childCount: tileCount * 2 - 1),
    );
  }
}

class _LimitEditor extends StatelessWidget {
  final TextEditingController controller;
  final FocusNode focusNode;
  final VoidCallback onSubmitted;

  const _LimitEditor({
    required this.controller,
    required this.focusNode,
    required this.onSubmitted,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return SizedBox(
      height: 48,
      child: TextField(
        controller: controller,
        focusNode: focusNode,
        keyboardType: TextInputType.number,
        inputFormatters: const [_DigitsOnlyTextInputFormatter()],
        decoration: InputDecoration(
          labelText: l.cacheMaxLimit,
          suffixText: 'MB',
          border: const OutlineInputBorder(),
          isDense: true,
        ),
        onSubmitted: (_) => onSubmitted(),
      ),
    );
  }
}

class _DigitsOnlyTextInputFormatter extends TextInputFormatter {
  const _DigitsOnlyTextInputFormatter();

  static final _digitsOnly = RegExp(r'^\d*$');

  @override
  TextEditingValue formatEditUpdate(
    TextEditingValue oldValue,
    TextEditingValue newValue,
  ) {
    if (_digitsOnly.hasMatch(newValue.text)) {
      return newValue.copyWith(composing: TextRange.empty);
    }
    return oldValue.copyWith(composing: TextRange.empty);
  }
}

class _CachePathRow extends StatelessWidget {
  final String path;
  final PathLauncher pathLauncher;

  const _CachePathRow({required this.path, required this.pathLauncher});

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return Row(
      children: [
        Expanded(
          child: Text(
            path,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: SettingsPageStyle.secondary(context),
          ),
        ),
        const SizedBox(width: 8),
        _SmallPathButton(
          icon: Icons.copy,
          tooltip: l.copyCachePath,
          onPressed: () {
            unawaited(Clipboard.setData(ClipboardData(text: path)));
          },
        ),
        const SizedBox(width: 4),
        _SmallPathButton(
          icon: Icons.folder_open,
          tooltip: l.openCachePath,
          onPressed: () {
            unawaited(pathLauncher.openFolder(path));
          },
        ),
      ],
    );
  }
}

class _SmallPathButton extends StatelessWidget {
  final IconData icon;
  final String tooltip;
  final VoidCallback? onPressed;
  final bool destructive;

  const _SmallPathButton({
    required this.icon,
    required this.tooltip,
    required this.onPressed,
    this.destructive = false,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return SizedBox(
      width: 32,
      height: 32,
      child: IconButton(
        onPressed: onPressed,
        icon: Icon(icon, size: 18),
        tooltip: tooltip,
        padding: EdgeInsets.zero,
        constraints: const BoxConstraints.tightFor(width: 32, height: 32),
        style: destructive
            ? _cacheDeleteIconButtonStyle(colorScheme)
            : IconButton.styleFrom(
                tapTargetSize: MaterialTapTargetSize.shrinkWrap,
              ),
      ),
    );
  }
}

ButtonStyle _cacheDeleteIconButtonStyle(ColorScheme colorScheme) {
  const size = Size.square(32);
  final warningStates = {
    WidgetState.hovered,
    WidgetState.focused,
    WidgetState.pressed,
  };
  return ButtonStyle(
    padding: const WidgetStatePropertyAll(EdgeInsets.zero),
    fixedSize: const WidgetStatePropertyAll(size),
    minimumSize: const WidgetStatePropertyAll(size),
    maximumSize: const WidgetStatePropertyAll(size),
    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
    shape: WidgetStatePropertyAll(
      RoundedRectangleBorder(borderRadius: BorderRadius.circular(4)),
    ),
    foregroundColor: WidgetStateProperty.resolveWith((states) {
      if (states.contains(WidgetState.disabled)) {
        return colorScheme.onSurfaceVariant.withValues(alpha: 0.38);
      }
      if (states.any(warningStates.contains)) return colorScheme.error;
      return colorScheme.onSurfaceVariant;
    }),
    backgroundColor: WidgetStateProperty.resolveWith((states) {
      if (states.contains(WidgetState.disabled)) return Colors.transparent;
      if (states.any(warningStates.contains)) {
        return colorScheme.error.withValues(alpha: 0.12);
      }
      return Colors.transparent;
    }),
    overlayColor: const WidgetStatePropertyAll(Colors.transparent),
  );
}

class _CacheListHeader extends StatelessWidget {
  final int selectedCount;
  final int selectableCount;
  final bool deleting;
  final VoidCallback onSelectAll;
  final VoidCallback onCancelSelection;
  final VoidCallback onDeleteSelected;

  const _CacheListHeader({
    required this.selectedCount,
    required this.selectableCount,
    required this.deleting,
    required this.onSelectAll,
    required this.onCancelSelection,
    required this.onDeleteSelected,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final hasSelection = selectedCount > 0;
    return SizedBox(
      height: 32,
      child: Row(
        children: [
          Expanded(
            child: Text(
              l.cachePerVideo,
              style: SettingsPageStyle.sectionTitle(context),
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
            ),
          ),
          if (hasSelection) ...[
            Text(
              l.cacheSelectedCount(selectedCount),
              style: SettingsPageStyle.secondary(context),
            ),
            const SizedBox(width: 8),
          ],
          _SmallPathButton(
            icon: Icons.select_all,
            tooltip: l.selectAll,
            onPressed: deleting || selectedCount >= selectableCount
                ? null
                : onSelectAll,
          ),
          const SizedBox(width: 4),
          _SmallPathButton(
            icon: Icons.close,
            tooltip: l.cancelSelection,
            onPressed: deleting || !hasSelection ? null : onCancelSelection,
          ),
          const SizedBox(width: 4),
          _SmallPathButton(
            icon: Icons.delete_outline,
            tooltip: l.deleteSelectedCache,
            onPressed: deleting || !hasSelection ? null : onDeleteSelected,
            destructive: true,
          ),
        ],
      ),
    );
  }
}

class _UsageSummary extends StatelessWidget {
  final AnalysisCacheSnapshot snapshot;

  const _UsageSummary({required this.snapshot});

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final overLimit = snapshot.isOverLimit;
    final color = overLimit
        ? theme.colorScheme.error
        : theme.colorScheme.primary;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            Icon(
              overLimit ? Icons.error_outline : Icons.storage,
              size: 18,
              color: color,
            ),
            const SizedBox(width: 8),
            Expanded(
              child: Text(
                snapshot.hasLimit
                    ? l.cacheUsageWithLimit(
                        AnalysisCache.formatBytes(snapshot.totalBytes),
                        AnalysisCache.formatBytes(snapshot.maxBytes),
                      )
                    : l.cacheUsageUnlimited(
                        AnalysisCache.formatBytes(snapshot.totalBytes),
                      ),
                style: SettingsPageStyle.body(context),
              ),
            ),
          ],
        ),
        if (snapshot.hasLimit) ...[
          const SizedBox(height: 8),
          LinearProgressIndicator(
            value: snapshot.usageFraction,
            color: color,
            backgroundColor: theme.colorScheme.surfaceContainerHighest,
          ),
          const SizedBox(height: 4),
          Text(
            overLimit
                ? l.cacheLimitReached
                : l.cacheRemaining(
                    AnalysisCache.formatBytes(snapshot.remainingBytes),
                  ),
            style:
                theme.textTheme.bodyMedium?.copyWith(
                  color: overLimit
                      ? theme.colorScheme.error
                      : theme.colorScheme.onSurfaceVariant,
                ) ??
                SettingsPageStyle.secondary(context),
          ),
        ],
      ],
    );
  }
}

class _CacheEntryTile extends StatelessWidget {
  final AnalysisCacheEntryStats entry;
  final bool selected;
  final ValueChanged<bool> onSelectedChanged;

  const _CacheEntryTile({
    required this.entry,
    required this.selected,
    required this.onSelectedChanged,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    return Material(
      type: MaterialType.transparency,
      child: ListTile(
        dense: true,
        contentPadding: EdgeInsets.zero,
        selected: selected,
        onTap: () => onSelectedChanged(!selected),
        leading: Checkbox(
          value: selected,
          onChanged: (value) => onSelectedChanged(value ?? false),
        ),
        title: Text(entry.name, maxLines: 1, overflow: TextOverflow.ellipsis),
        subtitle: Text(
          [
            l.cacheEntryBreakdown(
              AnalysisCache.formatBytes(entry.analysisBytes),
            ),
            if (entry.videoPath != null) entry.videoPath!,
          ].join('\n'),
          maxLines: 2,
          overflow: TextOverflow.ellipsis,
        ),
        trailing: Padding(
          padding: const EdgeInsets.only(right: _cacheListTrailingPadding),
          child: Text(
            AnalysisCache.formatBytes(entry.cacheBytes),
            style: theme.textTheme.labelLarge,
          ),
        ),
      ),
    );
  }
}

class _UnindexedCacheTile extends StatelessWidget {
  final int bytes;

  const _UnindexedCacheTile({required this.bytes});

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return Material(
      type: MaterialType.transparency,
      child: ListTile(
        dense: true,
        contentPadding: EdgeInsets.zero,
        leading: const SizedBox(width: 40, height: 48),
        title: Text(l.cacheUnindexed),
        trailing: Padding(
          padding: const EdgeInsets.only(right: _cacheListTrailingPadding),
          child: Text(AnalysisCache.formatBytes(bytes)),
        ),
      ),
    );
  }
}

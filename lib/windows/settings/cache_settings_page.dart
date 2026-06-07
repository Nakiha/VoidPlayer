import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../analysis/analysis_cache.dart';
import '../../feedback/app_feedback.dart';
import '../../l10n/app_localizations.dart';
import '../../platform/path_launcher.dart';
import '../../storage/app_storage.dart';
import '../../theme/app_appearance.dart';
import 'settings_page_style.dart';

const _cacheListTrailingPadding = 12.0;
const _cacheListScrollbarThickness = 6.0;
const _cacheTopControlsBottomGap = 8.0;
const _cachePinnedHeaderTopPadding = 6.0;
const _cachePinnedHeaderBottomPadding = 6.0;
const _cacheListHeaderExtent = 44.0;

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
  late final TextEditingController _analysisLimitController;
  late final TextEditingController _thumbnailLimitController;
  late final FocusNode _analysisLimitFocusNode;
  late final FocusNode _thumbnailLimitFocusNode;
  Timer? _refreshTimer;
  AppStorageSnapshot? _snapshot;
  final Set<String> _selectedAnalysisHashes = <String>{};
  final Set<String> _selectedAnnotationIds = <String>{};
  final Set<String> _selectedThumbnailIds = <String>{};
  bool _loading = false;
  bool _deletingAnalysis = false;
  bool _deletingAnnotations = false;
  bool _deletingThumbnails = false;

  @override
  void initState() {
    super.initState();
    final settings = AppSettingsScope.read(context);
    _analysisLimitController = TextEditingController(
      text: _limitInputFromBytes(settings.analysisCacheMaxBytes),
    );
    _thumbnailLimitController = TextEditingController(
      text: _limitInputFromBytes(settings.markThumbnailCacheMaxBytes),
    );
    _analysisLimitFocusNode = FocusNode()
      ..addListener(_onAnalysisLimitFocusChanged);
    _thumbnailLimitFocusNode = FocusNode()
      ..addListener(_onThumbnailLimitFocusChanged);
    _refresh();
    _refreshTimer = Timer.periodic(
      const Duration(seconds: 5),
      (_) => _refresh(),
    );
  }

  @override
  void dispose() {
    _refreshTimer?.cancel();
    if (_analysisLimitFocusNode.hasFocus) {
      unawaited(_saveAnalysisLimit(refresh: false));
    }
    if (_thumbnailLimitFocusNode.hasFocus) {
      unawaited(_saveThumbnailLimit(refresh: false));
    }
    _analysisLimitFocusNode.removeListener(_onAnalysisLimitFocusChanged);
    _thumbnailLimitFocusNode.removeListener(_onThumbnailLimitFocusChanged);
    _analysisLimitFocusNode.dispose();
    _thumbnailLimitFocusNode.dispose();
    _analysisLimitController.dispose();
    _thumbnailLimitController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final storage = _snapshot;

    return DefaultTabController(
      length: 3,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          SizedBox(
            height: 50,
            child: Stack(
              children: [
                Positioned.fill(
                  child: TabBar(
                    isScrollable: true,
                    tabAlignment: TabAlignment.start,
                    labelPadding: const EdgeInsets.symmetric(horizontal: 16),
                    indicatorSize: TabBarIndicatorSize.label,
                    dividerHeight: 1,
                    tabs: [
                      Tab(
                        height: 48,
                        child: _StorageTabLabel(
                          title: l.analysisCacheTab,
                          subtitle: _analysisTabSubtitle(storage?.analysis),
                        ),
                      ),
                      Tab(
                        height: 48,
                        child: _StorageTabLabel(
                          title: l.markDataTab,
                          subtitle: _bytesSubtitle(
                            storage?.marks.annotationBytes,
                          ),
                        ),
                      ),
                      Tab(
                        height: 48,
                        child: _StorageTabLabel(
                          title: l.markThumbnailCacheTab,
                          subtitle: _folderLimitSubtitle(
                            storage?.marks.thumbnails,
                          ),
                        ),
                      ),
                    ],
                  ),
                ),
                Positioned(
                  top: 9,
                  right: 8,
                  child: SizedBox(
                    width: 32,
                    height: 32,
                    child: IconButton(
                      onPressed: _loading ? null : _refresh,
                      icon: const Icon(Icons.refresh, size: 18),
                      tooltip: l.refresh,
                      padding: EdgeInsets.zero,
                      constraints: const BoxConstraints.tightFor(
                        width: 32,
                        height: 32,
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ),
          Expanded(
            child: TabBarView(
              children: [
                _AnalysisCacheTab(
                  snapshot: storage?.analysis,
                  selectedHashes: _selectedAnalysisHashes,
                  deleting: _deletingAnalysis,
                  limitController: _analysisLimitController,
                  limitFocusNode: _analysisLimitFocusNode,
                  pathLauncher: widget.pathLauncher,
                  onLimitSubmitted: () => _saveAnalysisLimit(),
                  onSelectAll: _selectAllAnalysisEntries,
                  onCancelSelection: _clearAnalysisSelection,
                  onDeleteSelected: _deleteSelectedAnalysis,
                  onSelectedChanged: _setAnalysisSelected,
                ),
                _StorageFilesTab(
                  snapshot: storage?.marks.annotations,
                  title: l.markDataFiles,
                  noEntriesText: l.markDataNoEntries,
                  selectedIds: _selectedAnnotationIds,
                  deleting: _deletingAnnotations,
                  pathLauncher: widget.pathLauncher,
                  deleteTooltip: l.deleteSelectedMarkData,
                  onSelectAll: _selectAllAnnotationEntries,
                  onCancelSelection: _clearAnnotationSelection,
                  onDeleteSelected: _deleteSelectedAnnotations,
                  onSelectedChanged: _setAnnotationSelected,
                ),
                _StorageFilesTab(
                  snapshot: storage?.marks.thumbnails,
                  title: l.markThumbnailFiles,
                  noEntriesText: l.markThumbnailNoEntries,
                  selectedIds: _selectedThumbnailIds,
                  deleting: _deletingThumbnails,
                  limitController: _thumbnailLimitController,
                  limitFocusNode: _thumbnailLimitFocusNode,
                  pathLauncher: widget.pathLauncher,
                  limitLabel: l.markThumbnailCacheMaxLimit,
                  limitReachedText: l.markThumbnailCacheLimitReached,
                  deleteTooltip: l.deleteSelectedMarkThumbnails,
                  onLimitSubmitted: () => _saveThumbnailLimit(),
                  onSelectAll: _selectAllThumbnailEntries,
                  onCancelSelection: _clearThumbnailSelection,
                  onDeleteSelected: _deleteSelectedThumbnails,
                  onSelectedChanged: _setThumbnailSelected,
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Future<void> _refresh() async {
    if (_loading) return;
    _loading = true;
    try {
      final settings = AppSettingsScope.read(context);
      final storage = await AppStorage.snapshot(
        analysisMaxBytes: settings.analysisCacheMaxBytes,
        markThumbnailMaxBytes: settings.markThumbnailCacheMaxBytes,
      );
      if (!mounted) return;
      setState(() {
        _snapshot = storage;
        final liveHashes = storage.analysis.entries.map((e) => e.hash).toSet();
        final liveAnnotations = storage.marks.annotations.entries
            .map((e) => e.id)
            .toSet();
        final liveThumbnails = storage.marks.thumbnails.entries
            .map((e) => e.id)
            .toSet();
        _selectedAnalysisHashes.removeWhere(
          (hash) => !liveHashes.contains(hash),
        );
        _selectedAnnotationIds.removeWhere(
          (id) => !liveAnnotations.contains(id),
        );
        _selectedThumbnailIds.removeWhere((id) => !liveThumbnails.contains(id));
      });
    } finally {
      _loading = false;
    }
  }

  void _onAnalysisLimitFocusChanged() {
    if (!_analysisLimitFocusNode.hasFocus) {
      unawaited(_saveAnalysisLimit());
    }
  }

  void _onThumbnailLimitFocusChanged() {
    if (!_thumbnailLimitFocusNode.hasFocus) {
      unawaited(_saveThumbnailLimit());
    }
  }

  Future<void> _saveAnalysisLimit({bool refresh = true}) async {
    final bytes = _bytesFromLimitInput(_analysisLimitController);
    final settings = AppSettingsScope.read(context);
    settings.analysisCacheMaxBytes = bytes;
    await settings.save();
    if (refresh && mounted) await _refresh();
  }

  Future<void> _saveThumbnailLimit({bool refresh = true}) async {
    final bytes = _bytesFromLimitInput(_thumbnailLimitController);
    final settings = AppSettingsScope.read(context);
    settings.markThumbnailCacheMaxBytes = bytes;
    await settings.save();
    if (refresh) {
      await AppStorage.enforceMarkThumbnailLimit(maxBytes: bytes);
    }
    if (refresh && mounted) await _refresh();
  }

  int _bytesFromLimitInput(TextEditingController controller) {
    final valueMb = int.tryParse(controller.text.trim()) ?? 0;
    if (controller.text != '$valueMb') {
      controller.text = '$valueMb';
    }
    return valueMb <= 0 ? 0 : valueMb * 1024 * 1024;
  }

  Future<void> _deleteSelectedAnalysis() async {
    if (_selectedAnalysisHashes.isEmpty || _deletingAnalysis) return;
    final l = AppLocalizations.of(context)!;
    final confirmed = await _confirmDeleteSelected(
      title: l.deleteSelectedCache,
      message: l.cacheDeleteConfirmMessage(_selectedAnalysisHashes.length),
    );
    if (!confirmed || !mounted) return;

    final hashes = _selectedAnalysisHashes.toSet();
    setState(() => _deletingAnalysis = true);
    AnalysisCacheDeleteResult? result;
    Object? error;
    try {
      result = await AnalysisCache.deleteEntries(hashes);
      if (!mounted) return;
      _selectedAnalysisHashes.removeAll(result.deletedHashes);
      await _refresh();
    } catch (e) {
      error = e;
    } finally {
      if (mounted) {
        setState(() => _deletingAnalysis = false);
      }
    }
    if (!mounted) return;

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

  static String _limitInputFromBytes(int bytes) {
    if (bytes <= 0) return '0';
    return (bytes / (1024 * 1024)).toStringAsFixed(0);
  }

  static String _analysisTabSubtitle(AnalysisCacheSnapshot? snapshot) {
    if (snapshot == null) return '';
    return snapshot.hasLimit
        ? '${AnalysisCache.formatBytes(snapshot.totalBytes)} / ${AnalysisCache.formatBytes(snapshot.maxBytes)}'
        : AnalysisCache.formatBytes(snapshot.totalBytes);
  }

  static String _folderLimitSubtitle(StorageFolderSnapshot? snapshot) {
    if (snapshot == null) return '';
    return snapshot.hasLimit
        ? '${AnalysisCache.formatBytes(snapshot.totalBytes)} / ${AnalysisCache.formatBytes(snapshot.maxBytes)}'
        : AnalysisCache.formatBytes(snapshot.totalBytes);
  }

  static String _bytesSubtitle(int? bytes) {
    if (bytes == null) return '';
    return AnalysisCache.formatBytes(bytes);
  }

  void _setAnalysisSelected(String hash, bool selected) {
    setState(() {
      if (selected) {
        _selectedAnalysisHashes.add(hash);
      } else {
        _selectedAnalysisHashes.remove(hash);
      }
    });
  }

  void _setAnnotationSelected(String id, bool selected) {
    setState(() {
      if (selected) {
        _selectedAnnotationIds.add(id);
      } else {
        _selectedAnnotationIds.remove(id);
      }
    });
  }

  void _setThumbnailSelected(String id, bool selected) {
    setState(() {
      if (selected) {
        _selectedThumbnailIds.add(id);
      } else {
        _selectedThumbnailIds.remove(id);
      }
    });
  }

  void _clearAnalysisSelection() {
    setState(_selectedAnalysisHashes.clear);
  }

  void _clearAnnotationSelection() {
    setState(_selectedAnnotationIds.clear);
  }

  void _clearThumbnailSelection() {
    setState(_selectedThumbnailIds.clear);
  }

  void _selectAllAnalysisEntries() {
    final entries = _snapshot?.analysis.entries;
    if (entries == null || entries.isEmpty) return;
    setState(() {
      _selectedAnalysisHashes
        ..clear()
        ..addAll(entries.map((entry) => entry.hash));
    });
  }

  void _selectAllAnnotationEntries() {
    final entries = _snapshot?.marks.annotations.entries;
    if (entries == null || entries.isEmpty) return;
    setState(() {
      _selectedAnnotationIds
        ..clear()
        ..addAll(entries.map((entry) => entry.id));
    });
  }

  void _selectAllThumbnailEntries() {
    final entries = _snapshot?.marks.thumbnails.entries;
    if (entries == null || entries.isEmpty) return;
    setState(() {
      _selectedThumbnailIds
        ..clear()
        ..addAll(entries.map((entry) => entry.id));
    });
  }

  Future<void> _deleteSelectedAnnotations() async {
    if (_selectedAnnotationIds.isEmpty || _deletingAnnotations) return;
    final l = AppLocalizations.of(context)!;
    final confirmed = await _confirmDeleteSelected(
      title: l.deleteSelectedMarkData,
      message: l.storageDeleteConfirmMessage(_selectedAnnotationIds.length),
    );
    if (!confirmed || !mounted) return;

    await _deleteStorageFiles(
      ids: _selectedAnnotationIds.toSet(),
      deletingSetter: (value) => _deletingAnnotations = value,
      delete: AppStorage.deleteMarkAnnotationFiles,
      clearDeleted: _selectedAnnotationIds.removeAll,
    );
  }

  Future<void> _deleteSelectedThumbnails() async {
    if (_selectedThumbnailIds.isEmpty || _deletingThumbnails) return;
    final l = AppLocalizations.of(context)!;
    final confirmed = await _confirmDeleteSelected(
      title: l.deleteSelectedMarkThumbnails,
      message: l.storageDeleteConfirmMessage(_selectedThumbnailIds.length),
    );
    if (!confirmed || !mounted) return;

    await _deleteStorageFiles(
      ids: _selectedThumbnailIds.toSet(),
      deletingSetter: (value) => _deletingThumbnails = value,
      delete: AppStorage.deleteMarkThumbnailFiles,
      clearDeleted: _selectedThumbnailIds.removeAll,
    );
  }

  Future<void> _deleteStorageFiles({
    required Set<String> ids,
    required void Function(bool value) deletingSetter,
    required Future<StorageDeleteResult> Function(Iterable<String> ids) delete,
    required void Function(Iterable<String> ids) clearDeleted,
  }) async {
    setState(() => deletingSetter(true));
    StorageDeleteResult? result;
    Object? error;
    try {
      result = await delete(ids);
      if (!mounted) return;
      clearDeleted(result.deletedIds);
      await _refresh();
    } catch (e) {
      error = e;
    } finally {
      if (mounted) {
        setState(() => deletingSetter(false));
      }
    }
    if (!mounted) return;

    final l = AppLocalizations.of(context)!;
    final deleteResult = result;
    final message = error != null
        ? l.storageDeleteFailed(ids.length)
        : deleteResult!.hasFailures
        ? l.storageDeleteFailed(deleteResult.failedCount)
        : l.storageDeleted(deleteResult.deletedCount);
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

  Future<bool> _confirmDeleteSelected({
    required String title,
    required String message,
  }) async {
    final l = AppLocalizations.of(context)!;
    final result = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(title),
        content: Text(message),
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

class _StorageTabLabel extends StatelessWidget {
  final String title;
  final String subtitle;

  const _StorageTabLabel({required this.title, required this.subtitle});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Column(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        Text(title, maxLines: 1, overflow: TextOverflow.ellipsis),
        const SizedBox(height: 1),
        Text(
          subtitle,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: theme.textTheme.labelSmall?.copyWith(
            color: theme.colorScheme.onSurfaceVariant,
          ),
        ),
      ],
    );
  }
}

class _StorageFilesTab extends StatelessWidget {
  final StorageFolderSnapshot? snapshot;
  final String title;
  final String noEntriesText;
  final Set<String> selectedIds;
  final bool deleting;
  final TextEditingController? limitController;
  final FocusNode? limitFocusNode;
  final PathLauncher pathLauncher;
  final String? limitLabel;
  final String? limitReachedText;
  final String deleteTooltip;
  final VoidCallback? onLimitSubmitted;
  final VoidCallback onSelectAll;
  final VoidCallback onCancelSelection;
  final VoidCallback onDeleteSelected;
  final void Function(String id, bool selected) onSelectedChanged;

  const _StorageFilesTab({
    required this.snapshot,
    required this.title,
    required this.noEntriesText,
    required this.selectedIds,
    required this.deleting,
    required this.pathLauncher,
    required this.deleteTooltip,
    required this.onSelectAll,
    required this.onCancelSelection,
    required this.onDeleteSelected,
    required this.onSelectedChanged,
    this.limitController,
    this.limitFocusNode,
    this.limitLabel,
    this.limitReachedText,
    this.onLimitSubmitted,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final snapshot = this.snapshot;
    final hasLimit =
        limitController != null &&
        limitFocusNode != null &&
        limitLabel != null &&
        onLimitSubmitted != null;
    return ScrollbarTheme(
      data: ScrollbarThemeData(
        thickness: WidgetStateProperty.all(_cacheListScrollbarThickness),
      ),
      child: Scrollbar(
        thumbVisibility: true,
        child: CustomScrollView(
          slivers: [
            SliverPadding(
              padding: const EdgeInsets.fromLTRB(
                16,
                12,
                16 + _cacheListTrailingPadding,
                _cacheTopControlsBottomGap,
              ),
              sliver: SliverList.list(
                children: [
                  if (snapshot == null)
                    const LinearProgressIndicator()
                  else ...[
                    _CachePathRow(
                      label: l.storageDirectoryLabel,
                      path: snapshot.path,
                      pathLauncher: pathLauncher,
                      copyTooltip: l.copyStoragePath,
                      openTooltip: l.openStoragePath,
                    ),
                    if (hasLimit) ...[
                      const SizedBox(height: 10),
                      _LimitEditor(
                        controller: limitController!,
                        focusNode: limitFocusNode!,
                        labelText: limitLabel!,
                        onSubmitted: onLimitSubmitted!,
                      ),
                      const SizedBox(height: 12),
                      _UsageSummary(
                        totalBytes: snapshot.totalBytes,
                        maxBytes: snapshot.maxBytes,
                        limitReachedText: limitReachedText!,
                      ),
                    ],
                  ],
                ],
              ),
            ),
            SliverPersistentHeader(
              pinned: true,
              delegate: _CacheListHeaderDelegate(
                title: title,
                deleteTooltip: deleteTooltip,
                selectedCount: selectedIds.length,
                selectableCount: snapshot?.entries.length ?? 0,
                deleting: deleting,
                onSelectAll: onSelectAll,
                onCancelSelection: onCancelSelection,
                onDeleteSelected: onDeleteSelected,
              ),
            ),
            SliverPadding(
              padding: const EdgeInsets.fromLTRB(
                16,
                0,
                16 + _cacheListTrailingPadding,
                16,
              ),
              sliver: _StorageEntriesSliver(
                entries: snapshot?.entries,
                noEntriesText: noEntriesText,
                selectedIds: selectedIds,
                onSelectedChanged: onSelectedChanged,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _AnalysisCacheTab extends StatelessWidget {
  final AnalysisCacheSnapshot? snapshot;
  final Set<String> selectedHashes;
  final bool deleting;
  final TextEditingController limitController;
  final FocusNode limitFocusNode;
  final PathLauncher pathLauncher;
  final VoidCallback onLimitSubmitted;
  final VoidCallback onSelectAll;
  final VoidCallback onCancelSelection;
  final VoidCallback onDeleteSelected;
  final void Function(String hash, bool selected) onSelectedChanged;

  const _AnalysisCacheTab({
    required this.snapshot,
    required this.selectedHashes,
    required this.deleting,
    required this.limitController,
    required this.limitFocusNode,
    required this.pathLauncher,
    required this.onLimitSubmitted,
    required this.onSelectAll,
    required this.onCancelSelection,
    required this.onDeleteSelected,
    required this.onSelectedChanged,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final snapshot = this.snapshot;
    return ScrollbarTheme(
      data: ScrollbarThemeData(
        thickness: WidgetStateProperty.all(_cacheListScrollbarThickness),
      ),
      child: Scrollbar(
        thumbVisibility: true,
        child: CustomScrollView(
          slivers: [
            SliverPadding(
              padding: const EdgeInsets.fromLTRB(
                16,
                12,
                16 + _cacheListTrailingPadding,
                _cacheTopControlsBottomGap,
              ),
              sliver: SliverList.list(
                children: [
                  if (snapshot == null)
                    const LinearProgressIndicator()
                  else ...[
                    _CachePathRow(
                      label: l.storageDirectoryLabel,
                      path: snapshot.path,
                      pathLauncher: pathLauncher,
                      copyTooltip: l.copyCachePath,
                      openTooltip: l.openCachePath,
                    ),
                    const SizedBox(height: 10),
                    _LimitEditor(
                      controller: limitController,
                      focusNode: limitFocusNode,
                      labelText: l.rebuildableCacheMaxLimit,
                      onSubmitted: onLimitSubmitted,
                    ),
                    const SizedBox(height: 12),
                    _UsageSummary(
                      totalBytes: snapshot.totalBytes,
                      maxBytes: snapshot.maxBytes,
                      limitReachedText: l.cacheLimitReached,
                    ),
                  ],
                ],
              ),
            ),
            SliverPersistentHeader(
              pinned: true,
              delegate: _CacheListHeaderDelegate(
                title: l.cachePerVideo,
                deleteTooltip: l.deleteSelectedCache,
                selectedCount: selectedHashes.length,
                selectableCount: snapshot?.entries.length ?? 0,
                deleting: deleting,
                onSelectAll: onSelectAll,
                onCancelSelection: onCancelSelection,
                onDeleteSelected: onDeleteSelected,
              ),
            ),
            SliverPadding(
              padding: const EdgeInsets.fromLTRB(
                16,
                0,
                16 + _cacheListTrailingPadding,
                16,
              ),
              sliver: _CacheEntriesSliver(
                snapshot: snapshot,
                selectedHashes: selectedHashes,
                onSelectedChanged: onSelectedChanged,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _CacheListHeaderDelegate extends SliverPersistentHeaderDelegate {
  final String title;
  final String deleteTooltip;
  final int selectedCount;
  final int selectableCount;
  final bool deleting;
  final VoidCallback onSelectAll;
  final VoidCallback onCancelSelection;
  final VoidCallback onDeleteSelected;

  const _CacheListHeaderDelegate({
    required this.title,
    required this.deleteTooltip,
    required this.selectedCount,
    required this.selectableCount,
    required this.deleting,
    required this.onSelectAll,
    required this.onCancelSelection,
    required this.onDeleteSelected,
  });

  @override
  double get minExtent => _cacheListHeaderExtent;

  @override
  double get maxExtent => _cacheListHeaderExtent;

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
        child: _CacheListHeader(
          title: title,
          deleteTooltip: deleteTooltip,
          selectedCount: selectedCount,
          selectableCount: selectableCount,
          deleting: deleting,
          onSelectAll: onSelectAll,
          onCancelSelection: onCancelSelection,
          onDeleteSelected: onDeleteSelected,
        ),
      ),
    );
  }

  @override
  bool shouldRebuild(covariant _CacheListHeaderDelegate oldDelegate) {
    return title != oldDelegate.title ||
        deleteTooltip != oldDelegate.deleteTooltip ||
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

class _StorageEntriesSliver extends StatelessWidget {
  final List<StorageFileEntry>? entries;
  final String noEntriesText;
  final Set<String> selectedIds;
  final void Function(String id, bool selected) onSelectedChanged;

  const _StorageEntriesSliver({
    required this.entries,
    required this.noEntriesText,
    required this.selectedIds,
    required this.onSelectedChanged,
  });

  @override
  Widget build(BuildContext context) {
    final entries = this.entries;

    if (entries == null || entries.isEmpty) {
      return SliverFillRemaining(
        hasScrollBody: false,
        child: Center(child: Text(noEntriesText)),
      );
    }

    return SliverList(
      delegate: SliverChildBuilderDelegate((context, index) {
        if (index.isOdd) return const Divider(height: 1);
        final entry = entries[index ~/ 2];
        return _StorageEntryTile(
          entry: entry,
          selected: selectedIds.contains(entry.id),
          onSelectedChanged: (selected) {
            onSelectedChanged(entry.id, selected);
          },
        );
      }, childCount: entries.length * 2 - 1),
    );
  }
}

class _LimitEditor extends StatelessWidget {
  final TextEditingController controller;
  final FocusNode focusNode;
  final String labelText;
  final VoidCallback onSubmitted;

  const _LimitEditor({
    required this.controller,
    required this.focusNode,
    required this.labelText,
    required this.onSubmitted,
  });

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      height: 48,
      child: TextField(
        controller: controller,
        focusNode: focusNode,
        keyboardType: TextInputType.number,
        inputFormatters: const [_DigitsOnlyTextInputFormatter()],
        decoration: InputDecoration(
          labelText: labelText,
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
  final String label;
  final String path;
  final PathLauncher pathLauncher;
  final String copyTooltip;
  final String openTooltip;

  const _CachePathRow({
    required this.label,
    required this.path,
    required this.pathLauncher,
    required this.copyTooltip,
    required this.openTooltip,
  });

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        SizedBox(
          width: 44,
          child: Text(
            label,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: SettingsPageStyle.secondary(context),
          ),
        ),
        const SizedBox(width: 8),
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
          tooltip: copyTooltip,
          onPressed: () {
            unawaited(Clipboard.setData(ClipboardData(text: path)));
          },
        ),
        const SizedBox(width: 4),
        _SmallPathButton(
          icon: Icons.folder_open,
          tooltip: openTooltip,
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
  final String title;
  final String deleteTooltip;
  final int selectedCount;
  final int selectableCount;
  final bool deleting;
  final VoidCallback onSelectAll;
  final VoidCallback onCancelSelection;
  final VoidCallback onDeleteSelected;

  const _CacheListHeader({
    required this.title,
    required this.deleteTooltip,
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
              title,
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
            tooltip: deleteTooltip,
            onPressed: deleting || !hasSelection ? null : onDeleteSelected,
            destructive: true,
          ),
        ],
      ),
    );
  }
}

class _UsageSummary extends StatelessWidget {
  final int totalBytes;
  final int maxBytes;
  final String limitReachedText;

  const _UsageSummary({
    required this.totalBytes,
    required this.maxBytes,
    required this.limitReachedText,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final hasLimit = maxBytes > 0;
    final overLimit = hasLimit && totalBytes >= maxBytes;
    final usageFraction = hasLimit
        ? (totalBytes / maxBytes).clamp(0.0, 1.0)
        : 0.0;
    final remainingBytes = hasLimit
        ? (maxBytes - totalBytes).clamp(0, maxBytes)
        : 0;
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
                hasLimit
                    ? l.cacheUsageWithLimit(
                        AnalysisCache.formatBytes(totalBytes),
                        AnalysisCache.formatBytes(maxBytes),
                      )
                    : l.cacheUsageUnlimited(
                        AnalysisCache.formatBytes(totalBytes),
                      ),
                style: SettingsPageStyle.body(context),
              ),
            ),
          ],
        ),
        if (hasLimit) ...[
          const SizedBox(height: 8),
          LinearProgressIndicator(
            value: usageFraction,
            color: color,
            backgroundColor: theme.colorScheme.surfaceContainerHighest,
          ),
          const SizedBox(height: 4),
          Text(
            overLimit
                ? limitReachedText
                : l.cacheRemaining(AnalysisCache.formatBytes(remainingBytes)),
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

class _StorageEntryTile extends StatelessWidget {
  final StorageFileEntry entry;
  final bool selected;
  final ValueChanged<bool> onSelectedChanged;

  const _StorageEntryTile({
    required this.entry,
    required this.selected,
    required this.onSelectedChanged,
  });

  @override
  Widget build(BuildContext context) {
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
          entry.path,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
        ),
        trailing: Padding(
          padding: const EdgeInsets.only(right: _cacheListTrailingPadding),
          child: Text(
            AnalysisCache.formatBytes(entry.bytes),
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

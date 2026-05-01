import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../analysis/analysis_cache.dart';
import '../../config/app_config.dart';
import '../../l10n/app_localizations.dart';

class CacheSettingsPage extends StatefulWidget {
  const CacheSettingsPage({super.key});

  @override
  State<CacheSettingsPage> createState() => _CacheSettingsPageState();
}

class _CacheSettingsPageState extends State<CacheSettingsPage> {
  late final TextEditingController _limitController;
  Timer? _refreshTimer;
  AnalysisCacheSnapshot? _snapshot;
  final Set<String> _selectedHashes = <String>{};
  bool _loading = false;
  bool _deleting = false;

  @override
  void initState() {
    super.initState();
    _limitController = TextEditingController(
      text: _limitInputFromBytes(AppConfig.instance.analysisCacheMaxBytes),
    );
    _refresh();
    _refreshTimer = Timer.periodic(
      const Duration(seconds: 1),
      (_) => _refresh(),
    );
  }

  @override
  void dispose() {
    _refreshTimer?.cancel();
    _limitController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final snapshot = _snapshot;

    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Text(l.cache, style: theme.textTheme.titleMedium),
              const Spacer(),
              IconButton(
                onPressed: _loading ? null : _refresh,
                icon: const Icon(Icons.refresh, size: 18),
                tooltip: l.refresh,
              ),
            ],
          ),
          const SizedBox(height: 4),
          if (snapshot != null) _CachePathRow(path: snapshot.path),
          const SizedBox(height: 16),
          _LimitEditor(controller: _limitController, onSave: _saveLimit),
          const SizedBox(height: 16),
          if (snapshot == null)
            const LinearProgressIndicator()
          else
            _UsageSummary(snapshot: snapshot),
          const SizedBox(height: 12),
          _CacheListHeader(
            selectedCount: _selectedHashes.length,
            deleting: _deleting,
            onCancelSelection: _clearSelection,
            onDeleteSelected: _deleteSelected,
          ),
          const SizedBox(height: 8),
          Expanded(
            child: snapshot == null || snapshot.entries.isEmpty
                ? Center(child: Text(l.cacheNoEntries))
                : ListView.separated(
                    itemCount:
                        snapshot.entries.length +
                        (snapshot.unindexedBytes > 0 ? 1 : 0),
                    separatorBuilder: (_, _) => const Divider(height: 1),
                    itemBuilder: (context, index) {
                      if (index >= snapshot.entries.length) {
                        return _UnindexedCacheTile(
                          bytes: snapshot.unindexedBytes,
                        );
                      }
                      final entry = snapshot.entries[index];
                      return _CacheEntryTile(
                        entry: entry,
                        selected: _selectedHashes.contains(entry.hash),
                        onSelectedChanged: (selected) {
                          _setSelected(entry.hash, selected);
                        },
                      );
                    },
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
      final snapshot = await AnalysisCache.snapshot(
        maxBytes: AppConfig.instance.analysisCacheMaxBytes,
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

  Future<void> _saveLimit() async {
    final valueMb = double.tryParse(_limitController.text.trim());
    final bytes = valueMb == null || valueMb <= 0
        ? 0
        : (valueMb * 1024 * 1024).round();
    AppConfig.instance.analysisCacheMaxBytes = bytes;
    await AppConfig.instance.save();
    await _refresh();
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

  Future<void> _deleteSelected() async {
    if (_selectedHashes.isEmpty || _deleting) return;
    final confirmed = await _confirmDeleteSelected();
    if (!confirmed || !mounted) return;

    final hashes = _selectedHashes.toSet();
    setState(() => _deleting = true);
    final result = await AnalysisCache.deleteEntries(hashes);
    if (!mounted) return;

    setState(() {
      _deleting = false;
      _selectedHashes.removeAll(result.deletedHashes);
    });
    await _refresh();
    if (!mounted) return;

    final l = AppLocalizations.of(context)!;
    final message = result.hasFailures
        ? l.cacheDeleteFailed(result.failedCount)
        : l.cacheDeleted(result.deletedCount);
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text(message)));
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

class _LimitEditor extends StatelessWidget {
  final TextEditingController controller;
  final Future<void> Function() onSave;

  const _LimitEditor({required this.controller, required this.onSave});

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return Row(
      crossAxisAlignment: CrossAxisAlignment.center,
      children: [
        Expanded(
          child: SizedBox(
            height: 48,
            child: TextField(
              controller: controller,
              keyboardType: const TextInputType.numberWithOptions(
                decimal: true,
              ),
              decoration: InputDecoration(
                labelText: l.cacheMaxLimit,
                suffixText: 'MB',
                border: const OutlineInputBorder(),
                isDense: true,
              ),
              onSubmitted: (_) => onSave(),
            ),
          ),
        ),
        const SizedBox(width: 8),
        SizedBox(
          width: 48,
          height: 48,
          child: FilledButton(
            onPressed: onSave,
            style: FilledButton.styleFrom(
              fixedSize: const Size(48, 48),
              minimumSize: const Size(48, 48),
              padding: EdgeInsets.zero,
              shape: RoundedRectangleBorder(
                borderRadius: BorderRadius.circular(8),
              ),
              tapTargetSize: MaterialTapTargetSize.shrinkWrap,
            ),
            child: Tooltip(
              message: l.save,
              child: const Icon(Icons.save, size: 18),
            ),
          ),
        ),
      ],
    );
  }
}

class _CachePathRow extends StatelessWidget {
  final String path;

  const _CachePathRow({required this.path});

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    return Row(
      children: [
        Expanded(
          child: Text(
            path,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: theme.textTheme.bodySmall?.copyWith(
              color: theme.colorScheme.onSurfaceVariant,
            ),
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
            unawaited(_openPath(path));
          },
        ),
      ],
    );
  }

  Future<void> _openPath(String path) async {
    await Directory(path).create(recursive: true);
    await Process.start('explorer.exe', [path]);
  }
}

class _SmallPathButton extends StatelessWidget {
  final IconData icon;
  final String tooltip;
  final VoidCallback? onPressed;

  const _SmallPathButton({
    required this.icon,
    required this.tooltip,
    required this.onPressed,
  });

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 32,
      height: 32,
      child: IconButton(
        onPressed: onPressed,
        icon: Icon(icon, size: 18),
        tooltip: tooltip,
        padding: EdgeInsets.zero,
        constraints: const BoxConstraints.tightFor(width: 32, height: 32),
      ),
    );
  }
}

class _CacheListHeader extends StatelessWidget {
  final int selectedCount;
  final bool deleting;
  final VoidCallback onCancelSelection;
  final VoidCallback onDeleteSelected;

  const _CacheListHeader({
    required this.selectedCount,
    required this.deleting,
    required this.onCancelSelection,
    required this.onDeleteSelected,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final hasSelection = selectedCount > 0;
    return SizedBox(
      height: 32,
      child: Row(
        children: [
          Expanded(
            child: Text(
              l.cachePerVideo,
              style: theme.textTheme.titleSmall,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
            ),
          ),
          if (hasSelection) ...[
            Text(
              l.cacheSelectedCount(selectedCount),
              style: theme.textTheme.bodySmall?.copyWith(
                color: theme.colorScheme.onSurfaceVariant,
              ),
            ),
            const SizedBox(width: 8),
            _SmallPathButton(
              icon: Icons.close,
              tooltip: l.cancelSelection,
              onPressed: deleting ? null : onCancelSelection,
            ),
            const SizedBox(width: 4),
            _SmallPathButton(
              icon: Icons.delete_outline,
              tooltip: l.deleteSelectedCache,
              onPressed: deleting ? null : onDeleteSelected,
            ),
          ],
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
                style: theme.textTheme.bodyMedium,
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
            style: theme.textTheme.bodySmall?.copyWith(
              color: overLimit
                  ? theme.colorScheme.error
                  : theme.colorScheme.onSurfaceVariant,
            ),
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
    return ListTile(
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
            AnalysisCache.formatBytes(entry.vbs2Bytes),
            AnalysisCache.formatBytes(entry.vbiBytes),
            AnalysisCache.formatBytes(entry.vbtBytes),
          ),
          if (entry.videoPath != null) entry.videoPath!,
        ].join('\n'),
        maxLines: 2,
        overflow: TextOverflow.ellipsis,
      ),
      trailing: Text(
        AnalysisCache.formatBytes(entry.cacheBytes),
        style: theme.textTheme.labelLarge,
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
    return ListTile(
      dense: true,
      contentPadding: EdgeInsets.zero,
      leading: const Icon(Icons.folder_copy_outlined),
      title: Text(l.cacheUnindexed),
      trailing: Text(AnalysisCache.formatBytes(bytes)),
    );
  }
}

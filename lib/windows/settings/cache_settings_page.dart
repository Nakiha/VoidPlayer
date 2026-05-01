import 'dart:async';

import 'package:flutter/material.dart';

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
  bool _loading = false;

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
          if (snapshot != null)
            Text(
              snapshot.path,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: theme.textTheme.bodySmall?.copyWith(
                color: theme.colorScheme.onSurfaceVariant,
              ),
            ),
          const SizedBox(height: 16),
          _LimitEditor(controller: _limitController, onSave: _saveLimit),
          const SizedBox(height: 16),
          if (snapshot == null)
            const LinearProgressIndicator()
          else
            _UsageSummary(snapshot: snapshot),
          const SizedBox(height: 12),
          Text(l.cachePerVideo, style: theme.textTheme.titleSmall),
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
                      return _CacheEntryTile(entry: snapshot.entries[index]);
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
      setState(() => _snapshot = snapshot);
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
}

class _LimitEditor extends StatelessWidget {
  final TextEditingController controller;
  final Future<void> Function() onSave;

  const _LimitEditor({required this.controller, required this.onSave});

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return Row(
      crossAxisAlignment: CrossAxisAlignment.end,
      children: [
        Expanded(
          child: TextField(
            controller: controller,
            keyboardType: const TextInputType.numberWithOptions(decimal: true),
            decoration: InputDecoration(
              labelText: l.cacheMaxLimit,
              helperText: l.cacheUnlimitedHint,
              suffixText: 'MB',
              border: const OutlineInputBorder(),
              isDense: true,
            ),
            onSubmitted: (_) => onSave(),
          ),
        ),
        const SizedBox(width: 8),
        IconButton.filled(
          onPressed: onSave,
          icon: const Icon(Icons.save, size: 18),
          tooltip: l.save,
        ),
      ],
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

  const _CacheEntryTile({required this.entry});

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    return ListTile(
      dense: true,
      contentPadding: EdgeInsets.zero,
      leading: Icon(
        entry.complete ? Icons.movie_filter : Icons.warning_amber,
        color: entry.complete ? null : theme.colorScheme.error,
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

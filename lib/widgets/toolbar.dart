import 'dart:async';

import 'package:flutter/material.dart';

import '../analysis/analysis_cache.dart';
import '../analysis/analysis_manager.dart';
import '../analysis/analysis_toolbar_data_source.dart';
import '../l10n/app_localizations.dart';
import '../track_manager.dart';
import 'segmented_widget.dart';

const _analysisPanelWidth = 360.0;
const _analysisPanelMaxHeight = 240.0;
const _analysisPanelHeaderHeight = 34.0;
const _analysisPanelRowHeight = 44.0;

/// Top toolbar matching PySide6 ToolBar (40px height, margins: 4).
class AppToolBar extends StatelessWidget {
  final int viewMode; // 0=sideBySide, 1=splitScreen
  final ValueChanged<int> onViewModeChanged;
  final VoidCallback onAddMedia;
  final Future<void> Function() onAnalysis;
  final VoidCallback onProfiler;
  final VoidCallback onSettings;
  final List<TrackEntry> tracks;
  final AnalysisToolbarDataSource analysisDataSource;
  final bool viewModeEnabled;
  final bool analysisEnabled;

  const AppToolBar({
    super.key,
    required this.viewMode,
    required this.onViewModeChanged,
    required this.onAddMedia,
    required this.onAnalysis,
    required this.onProfiler,
    required this.onSettings,
    required this.tracks,
    required this.analysisDataSource,
    this.viewModeEnabled = false,
    this.analysisEnabled = false,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      height: 40,
      padding: const EdgeInsets.all(4),
      child: Row(
        children: [
          // View mode selector (240x32)
          Opacity(
            opacity: viewModeEnabled ? 1.0 : 0.5,
            child: IgnorePointer(
              ignoring: !viewModeEnabled,
              child: ViewModeSelector(
                currentMode: viewMode,
                onChanged: onViewModeChanged,
              ),
            ),
          ),
          const Spacer(),
          // Add Media button
          SizedBox(
            height: 32,
            child: FilledButton.icon(
              onPressed: onAddMedia,
              icon: const Icon(Icons.add, size: 16),
              label: Text(AppLocalizations.of(context)!.addMedia),
              style: FilledButton.styleFrom(
                padding: const EdgeInsets.symmetric(horizontal: 12),
                visualDensity: VisualDensity.compact,
              ),
            ),
          ),
          const SizedBox(width: 4),
          // Analysis button
          _AnalysisButton(
            enabled: analysisEnabled,
            tracks: tracks,
            dataSource: analysisDataSource,
            onPressed: onAnalysis,
          ),
          const SizedBox(width: 4),
          // Profiler button
          SizedBox(
            width: 32,
            height: 32,
            child: IconButton(
              onPressed: onProfiler,
              icon: const Icon(Icons.speed, size: 18),
              tooltip: AppLocalizations.of(context)!.performanceMonitor,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints.tightFor(width: 32, height: 32),
            ),
          ),
          const SizedBox(width: 4),
          // Settings button
          SizedBox(
            width: 32,
            height: 32,
            child: IconButton(
              onPressed: onSettings,
              icon: const Icon(Icons.settings, size: 18),
              tooltip: AppLocalizations.of(context)!.settings,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints.tightFor(width: 32, height: 32),
            ),
          ),
        ],
      ),
    );
  }
}

class _AnalysisButton extends StatefulWidget {
  final bool enabled;
  final List<TrackEntry> tracks;
  final AnalysisToolbarDataSource dataSource;
  final Future<void> Function() onPressed;

  const _AnalysisButton({
    required this.enabled,
    required this.tracks,
    required this.dataSource,
    required this.onPressed,
  });

  @override
  State<_AnalysisButton> createState() => _AnalysisButtonState();
}

class _AnalysisButtonState extends State<_AnalysisButton>
    with SingleTickerProviderStateMixin {
  final LayerLink _layerLink = LayerLink();
  OverlayEntry? _overlayEntry;
  Timer? _hideTimer;
  bool _hoveringButton = false;
  bool _hoveringPanel = false;
  late final AnimationController _panelAnimationController;
  late final Animation<double> _panelOpacity;

  @override
  void initState() {
    super.initState();
    _panelAnimationController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 130),
      reverseDuration: const Duration(milliseconds: 110),
    );
    _panelOpacity = CurvedAnimation(
      parent: _panelAnimationController,
      curve: Curves.easeOutCubic,
      reverseCurve: Curves.easeInCubic,
    );
  }

  @override
  void didUpdateWidget(covariant _AnalysisButton oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (_overlayEntry != null && oldWidget.tracks != widget.tracks) {
      _overlayEntry!.markNeedsBuild();
    }
  }

  @override
  void dispose() {
    _hideTimer?.cancel();
    _removePanel();
    _panelAnimationController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MouseRegion(
      onEnter: (_) {
        _hoveringButton = true;
        _showPanel();
      },
      onExit: (_) {
        _hoveringButton = false;
        _scheduleHidePanel();
      },
      child: CompositedTransformTarget(
        link: _layerLink,
        child: ListenableBuilder(
          listenable: widget.dataSource,
          builder: (context, _) {
            final theme = Theme.of(context);
            final isWorking =
                widget.dataSource.state == AnalysisState.computingHash ||
                widget.dataSource.state == AnalysisState.generating;
            final isError = widget.dataSource.state == AnalysisState.error;

            return SizedBox(
              width: 32,
              height: 32,
              child: IconButton(
                onPressed: !widget.enabled || isWorking
                    ? null
                    : () => unawaited(_handlePressed()),
                icon: isWorking
                    ? const SizedBox(
                        width: 18,
                        height: 18,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : Icon(
                        isError
                            ? Icons.error_outline
                            : Icons.analytics_outlined,
                        size: 18,
                        color: isError ? theme.colorScheme.error : null,
                      ),
                padding: EdgeInsets.zero,
                constraints: const BoxConstraints.tightFor(
                  width: 32,
                  height: 32,
                ),
              ),
            );
          },
        ),
      ),
    );
  }

  Future<void> _handlePressed() async {
    _showPanel();
    await widget.onPressed();
    if (!mounted) return;
    final error = widget.dataSource.error;
    if (error == null) return;
    if (error.key != AnalysisErrorKey.cacheLimitExceeded &&
        error.key != AnalysisErrorKey.cacheWriteIncomplete) {
      return;
    }
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(SnackBar(content: Text(_errorText(context, error))));
  }

  void _showPanel() {
    _hideTimer?.cancel();
    if (_overlayEntry != null) {
      _overlayEntry!.markNeedsBuild();
      _panelAnimationController.forward();
      return;
    }
    final overlay = Overlay.of(context);
    _overlayEntry = OverlayEntry(
      builder: (context) => CompositedTransformFollower(
        link: _layerLink,
        targetAnchor: Alignment.bottomRight,
        followerAnchor: Alignment.topRight,
        offset: const Offset(0, 4),
        showWhenUnlinked: false,
        child: FadeTransition(
          opacity: _panelOpacity,
          child: UnconstrainedBox(
            alignment: Alignment.topRight,
            child: MouseRegion(
              onEnter: (_) {
                _hoveringPanel = true;
                _hideTimer?.cancel();
              },
              onExit: (_) {
                _hoveringPanel = false;
                _scheduleHidePanel();
              },
              child: SizedBox(
                width: _analysisPanelWidth,
                child: _AnalysisHoverPanel(
                  tracks: widget.tracks,
                  dataSource: widget.dataSource,
                ),
              ),
            ),
          ),
        ),
      ),
    );
    overlay.insert(_overlayEntry!);
    _panelAnimationController.forward(from: 0);
  }

  void _scheduleHidePanel() {
    _hideTimer?.cancel();
    _hideTimer = Timer(const Duration(milliseconds: 180), () {
      if (_hoveringButton || _hoveringPanel) return;
      unawaited(_fadeOutPanel());
    });
  }

  Future<void> _fadeOutPanel() async {
    await _panelAnimationController.reverse();
    if (!mounted) return;
    if (_hoveringButton || _hoveringPanel) {
      await _panelAnimationController.forward();
      return;
    }
    _removePanel();
  }

  void _removePanel() {
    _overlayEntry?.remove();
    _overlayEntry = null;
    if (_panelAnimationController.isAnimating ||
        _panelAnimationController.value != 0) {
      _panelAnimationController.value = 0;
    }
  }

  String _errorText(BuildContext context, AnalysisError e) {
    final l = AppLocalizations.of(context)!;
    return switch (e.key) {
      AnalysisErrorKey.hashFailed => l.analysisErrorHashFailed(
        e.args.firstOrNull ?? '',
      ),
      AnalysisErrorKey.unsupported => l.analysisErrorUnsupported(
        e.args.firstOrNull ?? '',
      ),
      AnalysisErrorKey.loadFailed => l.analysisErrorLoadFailed(
        e.args.firstOrNull ?? '',
      ),
      AnalysisErrorKey.cacheLimitExceeded => l.analysisErrorCacheLimitExceeded(
        e.args.isNotEmpty ? e.args[0] : '',
        e.args.length > 1 ? e.args[1] : '',
      ),
      AnalysisErrorKey.cacheWriteIncomplete =>
        l.analysisErrorCacheWriteIncomplete(
          e.args.isNotEmpty ? e.args[0] : '',
          e.args.length > 1 ? e.args[1] : '',
          e.args.length > 2 ? e.args[2] : '',
        ),
    };
  }
}

class _AnalysisHoverPanel extends StatefulWidget {
  final List<TrackEntry> tracks;
  final AnalysisToolbarDataSource dataSource;

  const _AnalysisHoverPanel({required this.tracks, required this.dataSource});

  @override
  State<_AnalysisHoverPanel> createState() => _AnalysisHoverPanelState();
}

class _AnalysisHoverPanelState extends State<_AnalysisHoverPanel> {
  Timer? _refreshTimer;
  AnalysisCacheSnapshot? _snapshot;
  Map<String, int> _bytesByHash = const {};
  bool _refreshing = false;

  @override
  void initState() {
    super.initState();
    widget.dataSource.addListener(_refresh);
    unawaited(_refresh());
    _refreshTimer = Timer.periodic(
      const Duration(milliseconds: 700),
      (_) => unawaited(_refresh()),
    );
  }

  @override
  void didUpdateWidget(covariant _AnalysisHoverPanel oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.tracks != widget.tracks) unawaited(_refresh());
  }

  @override
  void dispose() {
    _refreshTimer?.cancel();
    widget.dataSource.removeListener(_refresh);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final snapshot = _snapshot;

    return Material(
      elevation: 12,
      color: theme.colorScheme.surface,
      borderRadius: BorderRadius.circular(8),
      clipBehavior: Clip.antiAlias,
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxHeight: _analysisPanelMaxHeight),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            SizedBox(
              height: _analysisPanelHeaderHeight,
              child: Row(
                children: [
                  const SizedBox(width: 10),
                  Icon(
                    Icons.analytics_outlined,
                    size: 16,
                    color: theme.colorScheme.primary,
                  ),
                  const SizedBox(width: 6),
                  Expanded(
                    child: Text(
                      l.analysisCachePanelTitle,
                      style: theme.textTheme.labelLarge,
                    ),
                  ),
                  if (snapshot != null)
                    Padding(
                      padding: const EdgeInsets.only(right: 10),
                      child: Text(
                        snapshot.hasLimit
                            ? l.cacheUsageWithLimit(
                                widget.dataSource.formatBytes(
                                  snapshot.totalBytes,
                                ),
                                widget.dataSource.formatBytes(
                                  snapshot.maxBytes,
                                ),
                              )
                            : l.cacheUsageUnlimited(
                                widget.dataSource.formatBytes(
                                  snapshot.totalBytes,
                                ),
                              ),
                        style: theme.textTheme.labelMedium?.copyWith(
                          color: theme.colorScheme.onSurfaceVariant,
                        ),
                      ),
                    ),
                ],
              ),
            ),
            const Divider(height: 1),
            if (widget.tracks.isEmpty)
              Padding(
                padding: const EdgeInsets.all(12),
                child: Text(
                  l.analysisCachePanelEmpty,
                  style: theme.textTheme.bodyMedium?.copyWith(
                    color: theme.colorScheme.onSurfaceVariant,
                  ),
                ),
              )
            else
              Flexible(
                child: ListView.separated(
                  shrinkWrap: true,
                  padding: EdgeInsets.zero,
                  itemCount: widget.tracks.length,
                  separatorBuilder: (_, _) => const Divider(height: 1),
                  itemBuilder: (context, index) {
                    return SizedBox(
                      height: _analysisPanelRowHeight,
                      child: _AnalysisTrackCacheTile(
                        track: widget.tracks[index],
                        snapshot: snapshot,
                        bytesByHash: _bytesByHash,
                        dataSource: widget.dataSource,
                      ),
                    );
                  },
                ),
              ),
          ],
        ),
      ),
    );
  }

  Future<void> _refresh() async {
    if (_refreshing) return;
    _refreshing = true;
    try {
      final snapshot = await widget.dataSource.snapshot();
      final hashes = <String>{};
      for (final track in widget.tracks) {
        final statusHash = widget.dataSource.statusForPath(track.path)?.hash;
        if (statusHash != null) hashes.add(statusHash);
        for (final entry in snapshot.entries) {
          if (entry.videoPath == track.path) hashes.add(entry.hash);
        }
      }
      final bytesByHash = await widget.dataSource.currentBytesByHash(hashes);
      if (!mounted) return;
      setState(() {
        _snapshot = snapshot;
        _bytesByHash = bytesByHash;
      });
    } finally {
      _refreshing = false;
    }
  }
}

class _AnalysisTrackCacheTile extends StatelessWidget {
  final TrackEntry track;
  final AnalysisCacheSnapshot? snapshot;
  final Map<String, int> bytesByHash;
  final AnalysisToolbarDataSource dataSource;

  const _AnalysisTrackCacheTile({
    required this.track,
    required this.snapshot,
    required this.bytesByHash,
    required this.dataSource,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final status = dataSource.statusForPath(track.path);
    final cacheEntry = _cacheEntry(status);
    final hash = status?.hash ?? cacheEntry?.hash;
    final cacheBytes = hash == null
        ? cacheEntry?.analysisBytes ?? 0
        : bytesByHash[hash] ?? cacheEntry?.analysisBytes ?? 0;
    final working = status?.isWorking ?? false;
    final failed = status?.isError ?? false;
    final cached =
        (cacheEntry?.complete ?? false) || (status?.isCached ?? false);
    final color = failed
        ? theme.colorScheme.error
        : cached
        ? Colors.green
        : theme.colorScheme.onSurfaceVariant;

    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
      child: Row(
        children: [
          SizedBox(
            width: 20,
            height: 20,
            child: working
                ? const Padding(
                    padding: EdgeInsets.all(3),
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : Icon(
                    failed
                        ? Icons.error_outline
                        : cached
                        ? Icons.check_circle_outline
                        : Icons.storage_outlined,
                    size: 16,
                    color: color,
                  ),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  track.fileName,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: theme.textTheme.bodySmall?.copyWith(
                    color: failed ? theme.colorScheme.error : null,
                  ),
                ),
                Text(
                  _statusText(l, status, cached, cacheBytes),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: theme.textTheme.labelSmall?.copyWith(
                    color: failed
                        ? theme.colorScheme.error
                        : theme.colorScheme.onSurfaceVariant,
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(width: 8),
          Text(
            dataSource.formatBytes(cacheBytes),
            style: theme.textTheme.labelSmall?.copyWith(color: color),
          ),
        ],
      ),
    );
  }

  AnalysisCacheEntryStats? _cacheEntry(AnalysisTrackGenerationStatus? status) {
    final entries = snapshot?.entries ?? const <AnalysisCacheEntryStats>[];
    for (final entry in entries) {
      if (status?.hash != null && entry.hash == status!.hash) return entry;
      if (entry.videoPath == track.path) return entry;
    }
    return null;
  }

  String _statusText(
    AppLocalizations l,
    AnalysisTrackGenerationStatus? status,
    bool cached,
    int cacheBytes,
  ) {
    if (status?.isError ?? false) return l.analysisCacheStatusFailed;
    if (status?.status == AnalysisTrackStatus.computingHash) {
      return l.analysisCacheStatusChecking;
    }
    if (status?.status == AnalysisTrackStatus.generating) {
      return l.analysisCacheStatusGenerating(
        ((status!.progress.clamp(0.0, 1.0)) * 100).toStringAsFixed(0),
        dataSource.formatBytes(cacheBytes),
      );
    }
    if (status?.status == AnalysisTrackStatus.loading) {
      return l.analysisCacheStatusLoading;
    }
    if (cached) return l.analysisCacheStatusCached;
    return l.analysisCacheStatusMissing;
  }
}

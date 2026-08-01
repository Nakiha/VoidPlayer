import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../analysis/analysis_ffi.dart';
import '../analysis/analysis_quality_service.dart';
import '../analysis/ui/charts/analysis_chart_common.dart';
import '../analysis/ui/charts/analysis_frame_style.dart';
import '../app_log.dart';
import '../l10n/app_localizations.dart';
import '../track_manager.dart';
import 'main_window_view_model.dart';

const Key mainWindowQualityAnalyzeButtonKey = ValueKey(
  'main-window-quality-analyze',
);
const Key mainWindowQualityCreateMarksButtonKey = ValueKey(
  'main-window-quality-create-marks',
);
const Key mainWindowQualityChartKey = ValueKey('main-window-quality-chart');
const Key mainWindowQualityThresholdDragKey = ValueKey(
  'main-window-quality-threshold-drag',
);
const Key mainWindowQualityToolbarKey = ValueKey('main-window-quality-toolbar');
const Key mainWindowQualityMetricsKey = ValueKey('main-window-quality-metrics');

final _qualityUiLogger = appLogger('QualityUI');

/// Keeps completed quality reports alive while the analysis deck changes tabs.
///
/// The deck owns this session, so cached reports remain scoped to the current
/// main-window session instead of becoming global application state.
class MainWindowQualitySession {
  final Map<int, _MainWindowQualitySnapshot> _snapshots = {};

  _MainWindowQualitySnapshot? _restore(int? fileId) =>
      fileId == null ? null : _snapshots[fileId];

  void _save(int fileId, _MainWindowQualitySnapshot snapshot) {
    _snapshots[fileId] = snapshot;
  }
}

class _MainWindowQualitySnapshot {
  final AnalysisQualityMetric metric;
  final Set<AnalysisQualityMetric> visibleMetrics;
  final AnalysisQualityReport report;
  final Map<int, FrameInfo> framesByDecodedIndex;
  final AnalysisQualitySample? selectedSample;
  final double threshold;
  final String? notice;

  const _MainWindowQualitySnapshot({
    required this.metric,
    required this.visibleMetrics,
    required this.report,
    required this.framesByDecodedIndex,
    required this.selectedSample,
    required this.threshold,
    required this.notice,
  });
}

class MainWindowQualityDeck extends StatefulWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;
  final MainWindowQualitySession session;
  final int? selectedFileId;

  const MainWindowQualityDeck({
    super.key,
    required this.model,
    required this.actions,
    required this.session,
    this.selectedFileId,
  });

  @override
  State<MainWindowQualityDeck> createState() => _MainWindowQualityDeckState();
}

class _MainWindowQualityDeckState extends State<MainWindowQualityDeck> {
  int? _fileId;
  AnalysisQualityMetric _metric = AnalysisQualityMetric.blockiness;
  final Set<AnalysisQualityMetric> _visibleMetrics = {
    ...AnalysisQualityMetric.values,
  };
  AnalysisQualityReport? _report;
  Map<int, FrameInfo> _framesByDecodedIndex = const {};
  AnalysisQualitySample? _selectedSample;
  AnalysisQualitySample? _hoverSample;
  double _threshold = 0;
  bool _loading = false;
  String? _error;
  String? _notice;
  int _requestSerial = 0;
  AnalysisQualityProgress? _progress;
  AnalysisQualityCancellationToken? _cancelToken;

  @override
  void initState() {
    super.initState();
    _fileId = widget.selectedFileId ?? _initialFileId();
    _restoreReport();
  }

  @override
  void dispose() {
    _cancelToken?.cancel();
    _cancelToken = null;
    super.dispose();
  }

  @override
  void didUpdateWidget(covariant MainWindowQualityDeck oldWidget) {
    super.didUpdateWidget(oldWidget);
    final tracks = widget.model.media.tracks;
    if (widget.selectedFileId != null &&
        widget.selectedFileId != oldWidget.selectedFileId) {
      _fileId = widget.selectedFileId;
      _restoreReport();
      return;
    }
    if (_fileId == null || !tracks.any((entry) => entry.fileId == _fileId)) {
      _fileId = widget.selectedFileId ?? _initialFileId();
      _restoreReport();
    }
  }

  void _restoreReport() {
    // Switching tracks abandons any in-flight request: cancel it and bump the
    // serial so its completion cannot leak the old report into the newly
    // selected track or save it under the wrong file id.
    _cancelToken?.cancel();
    _cancelToken = null;
    _requestSerial++;
    _loading = false;
    _progress = null;
    final snapshot = widget.session._restore(_fileId);
    _metric = snapshot?.metric ?? AnalysisQualityMetric.blockiness;
    _visibleMetrics
      ..clear()
      ..addAll(snapshot?.visibleMetrics ?? AnalysisQualityMetric.values);
    _report = snapshot?.report;
    _framesByDecodedIndex = snapshot?.framesByDecodedIndex ?? const {};
    _selectedSample = snapshot?.selectedSample;
    _hoverSample = null;
    _threshold = snapshot?.threshold ?? 0;
    _error = null;
    _notice = snapshot?.notice;
  }

  void _saveReport() {
    final fileId = _fileId;
    final report = _report;
    if (fileId == null || report == null) return;
    widget.session._save(
      fileId,
      _MainWindowQualitySnapshot(
        metric: _metric,
        visibleMetrics: Set.unmodifiable(_visibleMetrics),
        report: report,
        framesByDecodedIndex: Map.unmodifiable(_framesByDecodedIndex),
        selectedSample: _selectedSample,
        threshold: _threshold,
        notice: _notice,
      ),
    );
  }

  int? _initialFileId() {
    final tracks = widget.model.media.tracks;
    return tracks.isEmpty ? null : tracks.first.fileId;
  }

  TrackEntry? get _selectedTrack {
    for (final track in widget.model.media.tracks) {
      if (track.fileId == _fileId) return track;
    }
    return null;
  }

  Future<void> _analyze() async {
    final track = _selectedTrack;
    if (track == null || _loading) return;
    final serial = ++_requestSerial;
    final cancelToken = AnalysisQualityCancellationToken();
    setState(() {
      _loading = true;
      _error = null;
      _notice = null;
      _selectedSample = null;
      _hoverSample = null;
      _progress = null;
      _cancelToken = cancelToken;
    });
    try {
      final source = widget.model.deck.qualityDataSource;
      final report = await source.analyze(
        AnalysisQualityRequest(
          videoPath: track.path,
          sampleIntervalUs: 1000000,
          maxSamples: 600,
          cancellationToken: cancelToken,
          onProgress: (progress) {
            if (!mounted || serial != _requestSerial) return;
            setState(() => _progress = progress);
          },
        ),
      );
      Map<int, FrameInfo> frames = const {};
      if (source case final AnalysisQualityFrameDataSource frameSource) {
        try {
          frames = await frameSource.readCachedFrames(
            track.path,
            report.samples,
          );
        } catch (error, stackTrace) {
          _qualityUiLogger.warning(
            'cached frame enrichment failed file_id=${track.fileId} '
            'samples=${report.samples.length}',
            error,
            stackTrace,
          );
          frames = const {};
        }
      }
      if (!mounted || serial != _requestSerial) return;
      setState(() {
        _report = report;
        _framesByDecodedIndex = frames;
        _threshold = _distribution(report, _metric).p95.clamp(0.0, 1.0);
        _loading = false;
        _progress = null;
        _cancelToken = null;
      });
      _saveReport();
    } catch (error) {
      if (!mounted || serial != _requestSerial) return;
      if (error is AnalysisQualityException && error.code == 'cancelled') {
        // Cooperative cancellation restores the previous report unchanged.
        setState(() {
          _loading = false;
          _progress = null;
          _cancelToken = null;
        });
        return;
      }
      setState(() {
        _report = null;
        _framesByDecodedIndex = const {};
        _loading = false;
        _progress = null;
        _cancelToken = null;
        _error = error.toString();
      });
    }
  }

  void _cancelAnalysis() {
    _cancelToken?.cancel();
  }

  String _progressLabel(AppLocalizations l) {
    final fraction = _progress?.fraction;
    if (fraction == null) return l.qualityAnalyzing;
    return '${l.qualityAnalyzing} ${(fraction * 100).round()}%';
  }

  void _selectMetric(AnalysisQualityMetric metric) {
    final report = _report;
    setState(() {
      _metric = metric;
      _visibleMetrics.add(metric);
      _selectedSample = null;
      _notice = null;
      if (report != null) {
        _threshold = _distribution(report, metric).p95.clamp(0.0, 1.0);
      }
    });
    _saveReport();
  }

  void _toggleMetric(AnalysisQualityMetric metric) {
    setState(() {
      if (!_visibleMetrics.remove(metric)) _visibleMetrics.add(metric);
      _notice = null;
    });
    _saveReport();
  }

  Future<void> _createMarks() async {
    final report = _report;
    final fileId = _fileId;
    final create = widget.actions.deck.onQualityMarksRequested;
    if (report == null || fileId == null || create == null) return;
    _qualityUiLogger.info(
      'mark generation started file_id=$fileId metric=${_metric.name} '
      'threshold=${_threshold.toStringAsFixed(4)}',
    );
    final int created;
    try {
      created = await create(
        MainWindowQualityMarkRequest(
          fileId: fileId,
          metric: _metric,
          threshold: _threshold,
          report: report,
        ),
      );
    } catch (error, stackTrace) {
      _qualityUiLogger.warning(
        'mark generation failed file_id=$fileId metric=${_metric.name}',
        error,
        stackTrace,
      );
      rethrow;
    }
    _qualityUiLogger.info(
      'mark generation completed file_id=$fileId metric=${_metric.name} '
      'created=$created',
    );
    if (!mounted) return;
    final l = AppLocalizations.of(context)!;
    setState(() {
      _notice = created == 0
          ? l.qualityNoNewMarks
          : l.qualityCreatedMarks(created);
    });
    _saveReport();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    final tracks = widget.model.media.tracks;
    final report = _report;
    final samples = report?.samples ?? const <AnalysisQualitySample>[];
    final aboveThreshold = samples
        .where((sample) => (sample.valueFor(_metric) ?? -1) >= _threshold)
        .length;
    // CLI reports carry candidate events aggregated by the analyzer; the
    // legacy FFI path falls back to threshold-filtered samples.
    final eventCandidateCount = report != null && report.hasEventCandidates
        ? report.events.where((event) => event.metric == _metric).length
        : null;
    final markCandidateCount = eventCandidateCount ?? aboveThreshold;
    final distribution = report == null ? null : _distribution(report, _metric);

    return DecoratedBox(
      decoration: BoxDecoration(color: theme.colorScheme.surface),
      child: Padding(
        padding: const EdgeInsets.all(4),
        child: Column(
          children: [
            LayoutBuilder(
              builder: (context, constraints) {
                final statusWidth = (constraints.maxWidth * 0.18).clamp(
                  104.0,
                  220.0,
                );
                return SizedBox(
                  key: mainWindowQualityToolbarKey,
                  height: 32,
                  child: Row(
                    children: [
                      _QualityToolbarButton(
                        key: mainWindowQualityAnalyzeButtonKey,
                        onPressed: tracks.isEmpty
                            ? null
                            : _loading
                            ? _cancelAnalysis
                            : _analyze,
                        tooltip: _loading
                            ? l.qualityCancelAnalysis
                            : report == null
                            ? l.qualityAnalyze
                            : l.qualityReanalyze,
                        emphasized: report == null && !_loading,
                        icon: _loading
                            ? const SizedBox.square(
                                dimension: 13,
                                child: CircularProgressIndicator(
                                  strokeWidth: 2,
                                ),
                              )
                            : const Icon(Icons.query_stats, size: 16),
                      ),
                      const SizedBox(width: 4),
                      SizedBox(
                        width: report == null
                            ? constraints.maxWidth - 32
                            : statusWidth,
                        child: Text(
                          _loading
                              ? _progressLabel(l)
                              : report == null
                              ? l.qualityExperimentalProxyNotice
                              : l.qualitySampleSummary(
                                  report.videoWidth,
                                  report.videoHeight,
                                  report.samples.length,
                                  report.truncated
                                      ? l.qualitySampleCappedSuffix
                                      : '',
                                ),
                          overflow: TextOverflow.ellipsis,
                          maxLines: 1,
                          style: theme.textTheme.bodySmall?.copyWith(
                            color: theme.colorScheme.onSurfaceVariant,
                          ),
                        ),
                      ),
                      if (report != null) ...[
                        const SizedBox(width: 8),
                        Expanded(
                          child: SingleChildScrollView(
                            key: mainWindowQualityMetricsKey,
                            scrollDirection: Axis.horizontal,
                            child: Row(
                              children: [
                                for (final metric
                                    in AnalysisQualityMetric.values) ...[
                                  _QualityMetricControl(
                                    semanticsKey: ValueKey(
                                      'main-window-quality-metric-${metric.name}',
                                    ),
                                    label: _metricLabel(l, metric),
                                    color: _metricColor(metric),
                                    visible: _visibleMetrics.contains(metric),
                                    focused: _metric == metric,
                                    visibleLabel: l.qualityMetricVisible,
                                    hiddenLabel: l.qualityMetricHidden,
                                    onToggleVisibility: () =>
                                        _toggleMetric(metric),
                                    onSelect: () => _selectMetric(metric),
                                  ),
                                  const SizedBox(width: 4),
                                ],
                                if (distribution != null &&
                                    distribution.count > 0)
                                  Text(
                                    '${l.qualityMean} '
                                    '${distribution.mean.toStringAsFixed(3)}  ·  '
                                    '${l.qualityP95} '
                                    '${distribution.p95.toStringAsFixed(3)}',
                                    maxLines: 1,
                                    style: theme.textTheme.bodySmall?.copyWith(
                                      color: theme.colorScheme.onSurfaceVariant,
                                    ),
                                  ),
                              ],
                            ),
                          ),
                        ),
                        const SizedBox(width: 4),
                        _QualityToolbarButton(
                          key: mainWindowQualityCreateMarksButtonKey,
                          onPressed:
                              markCandidateCount == 0 ||
                                  widget.actions.deck.onQualityMarksRequested ==
                                      null
                              ? null
                              : _createMarks,
                          tooltip: l.qualityCreateMarks(markCandidateCount),
                          icon: const Icon(
                            Icons.add_location_alt_outlined,
                            size: 16,
                          ),
                        ),
                      ],
                    ],
                  ),
                );
              },
            ),
            const SizedBox(height: 4),
            Expanded(
              child: _buildChartArea(
                context,
                samples: samples,
                error: _error,
                loading: _loading,
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildChartArea(
    BuildContext context, {
    required List<AnalysisQualitySample> samples,
    required String? error,
    required bool loading,
  }) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    if (loading) {
      return Center(child: Text(l.qualityDecodingSamples));
    }
    if (error != null) {
      return Center(
        child: Text(
          l.qualityAnalysisFailed(error),
          maxLines: 3,
          overflow: TextOverflow.ellipsis,
          textAlign: TextAlign.center,
          style: TextStyle(color: theme.colorScheme.error),
        ),
      );
    }
    if (_report == null) {
      return Center(
        child: Text(
          l.qualityEmptyHint,
          style: theme.textTheme.bodySmall?.copyWith(
            color: theme.colorScheme.onSurfaceVariant,
          ),
        ),
      );
    }
    if (samples.isEmpty) {
      return Center(child: Text(l.qualityNoSamples));
    }

    final offsetUs = _fileId == null
        ? 0
        : widget.model.media.syncOffsets[_fileId] ?? 0;
    final playbackPtsUs = widget.model.playback.currentPtsUs - offsetUs;
    return LayoutBuilder(
      builder: (context, constraints) {
        final size = Size(constraints.maxWidth, constraints.maxHeight);
        final geometry = _QualityChartGeometry(size);
        final thresholdY = geometry.yForValue(_threshold);
        return Stack(
          children: [
            Positioned.fill(
              child: MouseRegion(
                onExit: (_) => setState(() => _hoverSample = null),
                onHover: (event) {
                  final sample = geometry.plotRect.contains(event.localPosition)
                      ? _nearestSample(
                          samples,
                          event.localPosition.dx,
                          geometry.plotRect,
                        )
                      : null;
                  if (!identical(sample, _hoverSample)) {
                    setState(() => _hoverSample = sample);
                  }
                },
                child: GestureDetector(
                  key: mainWindowQualityChartKey,
                  behavior: HitTestBehavior.opaque,
                  onTapDown: (details) {
                    if (!geometry.plotRect.contains(details.localPosition)) {
                      return;
                    }
                    final sample = _nearestSample(
                      samples,
                      details.localPosition.dx,
                      geometry.plotRect,
                    );
                    setState(() => _selectedSample = sample);
                    _saveReport();
                    final fileId = _fileId;
                    if (fileId != null) {
                      widget.actions.deck.onQualitySeekRequested?.call(
                        fileId,
                        sample.ptsUs,
                      );
                    }
                  },
                  child: CustomPaint(
                    painter: _QualityChartPainter(
                      samples: samples,
                      framesByDecodedIndex: _framesByDecodedIndex,
                      focusMetric: _metric,
                      visibleMetrics: Set.unmodifiable(_visibleMetrics),
                      threshold: _threshold,
                      playbackPtsUs: playbackPtsUs,
                      selectedSample: _selectedSample,
                      hoverSample: _hoverSample,
                      colors: theme.colorScheme,
                      metricLabels: {
                        for (final metric in AnalysisQualityMetric.values)
                          metric: _metricLabel(l, metric),
                      },
                      thresholdLabel: l.qualityThreshold,
                      seekHint: l.qualityClickChartToSeek,
                      notice: _notice,
                      tooltipTimeLabel: l.qualityTooltipTime,
                      tooltipQpLabel: l.qualityTooltipQp,
                      tooltipFrameSizeLabel: l.qualityTooltipFrameSize,
                      unavailableLabel: l.qualityUnavailable,
                    ),
                    size: Size.infinite,
                  ),
                ),
              ),
            ),
            Positioned(
              key: mainWindowQualityThresholdDragKey,
              left: geometry.plotRect.left,
              top: (thresholdY - 10).clamp(
                geometry.plotRect.top,
                math.max(geometry.plotRect.top, geometry.plotRect.bottom - 20),
              ),
              width: geometry.plotRect.width,
              height: 20,
              child: MouseRegion(
                cursor: SystemMouseCursors.resizeUpDown,
                child: GestureDetector(
                  behavior: HitTestBehavior.translucent,
                  onVerticalDragStart: (_) => setState(() => _notice = null),
                  onVerticalDragUpdate: (details) {
                    setState(() {
                      _threshold =
                          (_threshold -
                                  details.delta.dy / geometry.plotRect.height)
                              .clamp(0.0, 1.0);
                      _notice = null;
                    });
                  },
                  onVerticalDragEnd: (_) => _saveReport(),
                ),
              ),
            ),
          ],
        );
      },
    );
  }
}

class _QualityMetricControl extends StatelessWidget {
  final Key semanticsKey;
  final String label;
  final Color color;
  final bool visible;
  final bool focused;
  final String visibleLabel;
  final String hiddenLabel;
  final VoidCallback onToggleVisibility;
  final VoidCallback onSelect;

  const _QualityMetricControl({
    required this.semanticsKey,
    required this.label,
    required this.color,
    required this.visible,
    required this.focused,
    required this.visibleLabel,
    required this.hiddenLabel,
    required this.onToggleVisibility,
    required this.onSelect,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Semantics(
      key: semanticsKey,
      button: true,
      toggled: visible,
      selected: focused,
      label: '$label, ${visible ? visibleLabel : hiddenLabel}',
      child: Material(
        color: focused ? color.withValues(alpha: 0.13) : Colors.transparent,
        borderRadius: BorderRadius.circular(4),
        child: SizedBox(
          height: 28,
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              Tooltip(
                message: visible ? visibleLabel : hiddenLabel,
                child: InkResponse(
                  onTap: onToggleVisibility,
                  radius: 14,
                  child: SizedBox(
                    width: 24,
                    height: 28,
                    child: Center(
                      child: DecoratedBox(
                        decoration: BoxDecoration(
                          color: color.withValues(alpha: visible ? 1 : 0.24),
                          borderRadius: BorderRadius.circular(2),
                        ),
                        child: const SizedBox(width: 11, height: 9),
                      ),
                    ),
                  ),
                ),
              ),
              InkWell(
                onTap: onSelect,
                borderRadius: const BorderRadius.horizontal(
                  right: Radius.circular(4),
                ),
                child: Padding(
                  padding: const EdgeInsets.only(right: 8),
                  child: Text(
                    label,
                    maxLines: 1,
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: theme.colorScheme.onSurface.withValues(
                        alpha: visible ? 0.9 : 0.38,
                      ),
                      fontWeight: focused ? FontWeight.w700 : FontWeight.w500,
                    ),
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _QualityToolbarButton extends StatelessWidget {
  final VoidCallback? onPressed;
  final String tooltip;
  final Widget icon;
  final bool emphasized;

  const _QualityToolbarButton({
    super.key,
    required this.onPressed,
    required this.tooltip,
    required this.icon,
    this.emphasized = false,
  });

  @override
  Widget build(BuildContext context) {
    final colors = Theme.of(context).colorScheme;
    return Tooltip(
      message: tooltip,
      child: IconButton(
        onPressed: onPressed,
        icon: icon,
        padding: EdgeInsets.zero,
        visualDensity: VisualDensity.compact,
        constraints: const BoxConstraints.tightFor(width: 28, height: 28),
        style: IconButton.styleFrom(
          minimumSize: const Size.square(28),
          maximumSize: const Size.square(28),
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          backgroundColor: emphasized ? colors.secondaryContainer : null,
          foregroundColor: emphasized ? colors.onSecondaryContainer : null,
          disabledBackgroundColor: emphasized
              ? colors.onSurface.withValues(alpha: 0.08)
              : null,
        ),
      ),
    );
  }
}

AnalysisQualityDistribution _distribution(
  AnalysisQualityReport report,
  AnalysisQualityMetric metric,
) {
  return report.distributions[metric] ??
      const AnalysisQualityDistribution(count: 0, mean: 0, p95: 0, maximum: 0);
}

AnalysisQualitySample _nearestSample(
  List<AnalysisQualitySample> samples,
  double x,
  Rect plotRect,
) {
  if (samples.length == 1 || plotRect.width <= 0) return samples.first;
  final fraction = ((x - plotRect.left) / plotRect.width).clamp(0.0, 1.0);
  final firstPts = samples.first.ptsUs;
  final lastPts = samples.last.ptsUs;
  final targetPts = firstPts + ((lastPts - firstPts) * fraction).round();
  return samples.reduce(
    (best, sample) =>
        (sample.ptsUs - targetPts).abs() < (best.ptsUs - targetPts).abs()
        ? sample
        : best,
  );
}

String _metricLabel(AppLocalizations l, AnalysisQualityMetric metric) =>
    switch (metric) {
      AnalysisQualityMetric.blockiness => l.qualityMetricBlocking,
      AnalysisQualityMetric.banding => l.qualityMetricBanding,
      AnalysisQualityMetric.blur => l.qualityMetricBlur,
      AnalysisQualityMetric.noise => l.qualityMetricNoise,
      AnalysisQualityMetric.flicker => l.qualityMetricFlicker,
    };

Color _metricColor(AnalysisQualityMetric metric) => switch (metric) {
  AnalysisQualityMetric.blockiness => const Color(0xFFFF9800),
  AnalysisQualityMetric.banding => const Color(0xFFAB47BC),
  AnalysisQualityMetric.blur => const Color(0xFF42A5F5),
  AnalysisQualityMetric.noise => const Color(0xFF78909C),
  AnalysisQualityMetric.flicker => const Color(0xFFEF5350),
};

String _formatTimestamp(int ptsUs) {
  final totalMs = (ptsUs / 1000).round().clamp(0, 1 << 52);
  final minutes = totalMs ~/ 60000;
  final seconds = (totalMs % 60000) ~/ 1000;
  final millis = totalMs % 1000;
  return '$minutes:${seconds.toString().padLeft(2, '0')}.'
      '${millis.toString().padLeft(3, '0')}';
}

class _QualityChartGeometry {
  final Size size;
  late final Rect plotRect;

  _QualityChartGeometry(this.size) {
    const plotTop = 8.0;
    plotRect = Rect.fromLTRB(
      40,
      plotTop,
      math.max(41, size.width - 10),
      math.max(plotTop + 1, size.height - 27),
    );
  }

  double yForValue(double value) =>
      plotRect.bottom - value.clamp(0.0, 1.0) * plotRect.height;
}

class _QualityChartPainter extends CustomPainter {
  final List<AnalysisQualitySample> samples;
  final Map<int, FrameInfo> framesByDecodedIndex;
  final AnalysisQualityMetric focusMetric;
  final Set<AnalysisQualityMetric> visibleMetrics;
  final double threshold;
  final int playbackPtsUs;
  final AnalysisQualitySample? selectedSample;
  final AnalysisQualitySample? hoverSample;
  final ColorScheme colors;
  final Map<AnalysisQualityMetric, String> metricLabels;
  final String thresholdLabel;
  final String seekHint;
  final String? notice;
  final String tooltipTimeLabel;
  final String tooltipQpLabel;
  final String tooltipFrameSizeLabel;
  final String unavailableLabel;

  const _QualityChartPainter({
    required this.samples,
    required this.framesByDecodedIndex,
    required this.focusMetric,
    required this.visibleMetrics,
    required this.threshold,
    required this.playbackPtsUs,
    required this.selectedSample,
    required this.hoverSample,
    required this.colors,
    required this.metricLabels,
    required this.thresholdLabel,
    required this.seekHint,
    required this.notice,
    required this.tooltipTimeLabel,
    required this.tooltipQpLabel,
    required this.tooltipFrameSizeLabel,
    required this.unavailableLabel,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (samples.isEmpty || size.isEmpty) return;
    final geometry = _QualityChartGeometry(size);
    final plot = geometry.plotRect;
    if (plot.width <= 0 || plot.height <= 0) return;
    final firstPts = samples.first.ptsUs;
    final lastPts = samples.last.ptsUs;
    final duration = math.max(1, lastPts - firstPts);
    double xFor(int ptsUs) =>
        plot.left +
        ((ptsUs - firstPts) / duration).clamp(0.0, 1.0) * plot.width;
    double yFor(double value) =>
        plot.bottom - value.clamp(0.0, 1.0) * plot.height;

    _drawGrid(canvas, plot, xFor, firstPts, lastPts);

    canvas.save();
    canvas.clipRect(plot);
    _drawFrameBarcode(canvas, plot, xFor);
    _drawMetricCurves(canvas, xFor, yFor);
    _drawThreshold(canvas, plot, yFor(threshold));
    _drawPlaybackCursor(canvas, plot, xFor, firstPts, lastPts);
    _drawHover(canvas, plot, xFor, yFor);
    canvas.restore();

    _drawChartMessage(canvas, plot);
  }

  void _drawGrid(
    Canvas canvas,
    Rect plot,
    double Function(int) xFor,
    int firstPts,
    int lastPts,
  ) {
    final gridPaint = Paint()
      ..color = colors.outlineVariant.withValues(alpha: 0.35)
      ..strokeWidth = 1;
    final labelStyle = TextStyle(
      color: colors.onSurfaceVariant.withValues(alpha: 0.9),
      fontSize: 9,
    );
    for (var tick = 0; tick <= 4; tick++) {
      final value = tick / 4;
      final y = plot.bottom - value * plot.height;
      canvas.drawLine(Offset(plot.left, y), Offset(plot.right, y), gridPaint);
      _paintText(
        canvas,
        value.toStringAsFixed(2),
        Offset(3, y - 6),
        labelStyle,
        maxWidth: 34,
        textAlign: TextAlign.right,
      );
    }
    for (var tick = 0; tick <= 4; tick++) {
      final pts = firstPts + ((lastPts - firstPts) * tick / 4).round();
      final x = xFor(pts);
      canvas.drawLine(Offset(x, plot.top), Offset(x, plot.bottom), gridPaint);
      final label = _formatTimestamp(pts);
      final painter = _textPainter(label, labelStyle);
      painter.paint(
        canvas,
        Offset(
          (x - painter.width / 2).clamp(plot.left, plot.right - painter.width),
          plot.bottom + 5,
        ),
      );
    }
  }

  void _drawFrameBarcode(Canvas canvas, Rect plot, double Function(int) xFor) {
    if (framesByDecodedIndex.isEmpty) return;
    final available = [
      for (final sample in samples)
        if (framesByDecodedIndex[sample.decodedFrameIndex] != null)
          framesByDecodedIndex[sample.decodedFrameIndex]!,
    ];
    if (available.isEmpty) return;
    final maxBytes = available
        .map((frame) => frame.packetSize)
        .fold<int>(1, math.max);
    for (var index = 0; index < samples.length; index++) {
      final sample = samples[index];
      final frame = framesByDecodedIndex[sample.decodedFrameIndex];
      if (frame == null) continue;
      final center = xFor(sample.ptsUs);
      final left = index == 0
          ? plot.left
          : (xFor(samples[index - 1].ptsUs) + center) / 2;
      final right = index == samples.length - 1
          ? plot.right
          : (center + xFor(samples[index + 1].ptsUs)) / 2;
      final height =
          (frame.packetSize / maxBytes).clamp(0.0, 1.0) * plot.height;
      canvas.drawRect(
        Rect.fromLTRB(left, plot.bottom - height, right + 0.5, plot.bottom),
        Paint()..color = analysisFrameTypeColor(frame).withValues(alpha: 0.14),
      );
    }
  }

  void _drawMetricCurves(
    Canvas canvas,
    double Function(int) xFor,
    double Function(double) yFor,
  ) {
    for (final metric in AnalysisQualityMetric.values) {
      if (!visibleMetrics.contains(metric)) continue;
      final path = Path();
      var started = false;
      for (final sample in samples) {
        final value = sample.valueFor(metric);
        if (value == null) {
          started = false;
          continue;
        }
        final point = Offset(xFor(sample.ptsUs), yFor(value));
        if (started) {
          path.lineTo(point.dx, point.dy);
        } else {
          path.moveTo(point.dx, point.dy);
          started = true;
        }
      }
      canvas.drawPath(
        path,
        Paint()
          ..color = _metricColor(metric)
          ..style = PaintingStyle.stroke
          ..strokeWidth = metric == focusMetric ? 2.2 : 1.35
          ..strokeJoin = StrokeJoin.round,
      );
    }

    for (final sample in samples) {
      final value = sample.valueFor(focusMetric);
      if (value == null) continue;
      final selected = identical(sample, selectedSample);
      final over = value >= threshold;
      canvas.drawCircle(
        Offset(xFor(sample.ptsUs), yFor(value)),
        selected
            ? 4.5
            : over
            ? 3
            : 1.8,
        Paint()..color = over ? colors.error : _metricColor(focusMetric),
      );
      if (selected) {
        canvas.drawCircle(
          Offset(xFor(sample.ptsUs), yFor(value)),
          6,
          Paint()
            ..color = colors.onSurface
            ..style = PaintingStyle.stroke
            ..strokeWidth = 1,
        );
      }
    }
  }

  void _drawThreshold(Canvas canvas, Rect plot, double y) {
    final paint = Paint()
      ..color = colors.tertiary
      ..strokeWidth = 1.2;
    for (double x = plot.left; x < plot.right; x += 8) {
      canvas.drawLine(
        Offset(x, y),
        Offset(math.min(x + 4, plot.right), y),
        paint,
      );
    }
    _paintText(
      canvas,
      '$thresholdLabel ${threshold.toStringAsFixed(3)}',
      Offset(plot.left + 5, (y - 16).clamp(plot.top + 2, plot.bottom - 14)),
      TextStyle(color: colors.tertiary, fontSize: 9),
    );
  }

  void _drawPlaybackCursor(
    Canvas canvas,
    Rect plot,
    double Function(int) xFor,
    int firstPts,
    int lastPts,
  ) {
    if (playbackPtsUs < firstPts || playbackPtsUs > lastPts) return;
    final x = xFor(playbackPtsUs);
    canvas.drawLine(
      Offset(x, plot.top),
      Offset(x, plot.bottom),
      Paint()
        ..color = colors.onSurface.withValues(alpha: 0.8)
        ..strokeWidth = 1.2,
    );
  }

  void _drawHover(
    Canvas canvas,
    Rect plot,
    double Function(int) xFor,
    double Function(double) yFor,
  ) {
    final sample = hoverSample;
    if (sample == null) return;
    final x = xFor(sample.ptsUs);
    final focusValue = sample.valueFor(focusMetric) ?? 0;
    final y = yFor(focusValue);
    final crossPaint = Paint()
      ..color = colors.onSurface.withValues(alpha: 0.32)
      ..strokeWidth = 1;
    canvas.drawLine(Offset(x, plot.top), Offset(x, plot.bottom), crossPaint);
    canvas.drawLine(Offset(plot.left, y), Offset(plot.right, y), crossPaint);

    final frame = framesByDecodedIndex[sample.decodedFrameIndex];
    final qp = sample.averageQp ?? frame?.avgQp.toDouble();
    final lines = <String>[
      '$tooltipTimeLabel: ${_formatTimestamp(sample.ptsUs)}',
      for (final metric in AnalysisQualityMetric.values)
        '${metricLabels[metric]}: '
            '${sample.valueFor(metric)?.toStringAsFixed(3) ?? unavailableLabel}',
      '$tooltipQpLabel: ${qp?.toStringAsFixed(1) ?? unavailableLabel}',
      '$tooltipFrameSizeLabel: '
          '${frame == null ? unavailableLabel : formatBytes(frame.packetSize)}',
    ];
    _drawTooltip(canvas, plot, x, lines);
  }

  void _drawTooltip(Canvas canvas, Rect plot, double x, List<String> lines) {
    final style = TextStyle(color: colors.onInverseSurface, fontSize: 10);
    final painters = [for (final line in lines) _textPainter(line, style)];
    final width =
        painters.map((painter) => painter.width).fold<double>(0, math.max) + 14;
    final height = painters.fold<double>(
      10,
      (sum, painter) => sum + painter.height,
    );
    var left = x + 8;
    if (left + width > plot.right) left = x - width - 8;
    left = left.clamp(plot.left + 3, plot.right - width - 3);
    final top = plot.top + 4;
    final rect = RRect.fromRectAndRadius(
      Rect.fromLTWH(left, top, width, height),
      const Radius.circular(5),
    );
    canvas.drawRRect(rect, Paint()..color = colors.inverseSurface);
    var y = top + 5;
    for (final painter in painters) {
      painter.paint(canvas, Offset(left + 7, y));
      y += painter.height;
    }
  }

  void _drawChartMessage(Canvas canvas, Rect plot) {
    final text = notice ?? (selectedSample == null ? seekHint : null);
    if (text == null || hoverSample != null) return;
    final painter = _textPainter(
      text,
      TextStyle(color: colors.onSurfaceVariant, fontSize: 9),
    );
    painter.paint(
      canvas,
      Offset(plot.right - painter.width - 5, plot.bottom - painter.height - 4),
    );
  }

  static TextPainter _textPainter(
    String text,
    TextStyle style, {
    double? maxWidth,
    TextAlign textAlign = TextAlign.left,
  }) {
    final painter = TextPainter(
      text: TextSpan(text: text, style: style),
      textDirection: TextDirection.ltr,
      textAlign: textAlign,
      maxLines: 1,
      ellipsis: '…',
    );
    painter.layout(maxWidth: maxWidth ?? double.infinity);
    return painter;
  }

  static void _paintText(
    Canvas canvas,
    String text,
    Offset offset,
    TextStyle style, {
    double? maxWidth,
    TextAlign textAlign = TextAlign.left,
  }) {
    _textPainter(
      text,
      style,
      maxWidth: maxWidth,
      textAlign: textAlign,
    ).paint(canvas, offset);
  }

  @override
  bool shouldRepaint(covariant _QualityChartPainter oldDelegate) {
    return oldDelegate.samples != samples ||
        oldDelegate.framesByDecodedIndex != framesByDecodedIndex ||
        oldDelegate.focusMetric != focusMetric ||
        oldDelegate.visibleMetrics != visibleMetrics ||
        oldDelegate.threshold != threshold ||
        oldDelegate.playbackPtsUs != playbackPtsUs ||
        oldDelegate.selectedSample != selectedSample ||
        oldDelegate.hoverSample != hoverSample ||
        oldDelegate.colors != colors ||
        oldDelegate.notice != notice ||
        oldDelegate.metricLabels != metricLabels;
  }
}

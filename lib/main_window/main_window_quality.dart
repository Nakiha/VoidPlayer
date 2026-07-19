import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../analysis/analysis_ffi.dart';
import '../analysis/analysis_quality_service.dart';
import '../track_manager.dart';
import 'main_window_view_model.dart';

const Key mainWindowQualityAnalyzeButtonKey = ValueKey(
  'main-window-quality-analyze',
);
const Key mainWindowQualityCreateMarksButtonKey = ValueKey(
  'main-window-quality-create-marks',
);
const Key mainWindowQualityChartKey = ValueKey('main-window-quality-chart');

class MainWindowQualityDeck extends StatefulWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const MainWindowQualityDeck({
    super.key,
    required this.model,
    required this.actions,
  });

  @override
  State<MainWindowQualityDeck> createState() => _MainWindowQualityDeckState();
}

class _MainWindowQualityDeckState extends State<MainWindowQualityDeck> {
  int? _fileId;
  AnalysisQualityMetric _metric = AnalysisQualityMetric.blockiness;
  AnalysisQualityReport? _report;
  AnalysisQualitySample? _selectedSample;
  double _threshold = 0;
  bool _loading = false;
  String? _error;
  String? _notice;
  int _requestSerial = 0;

  @override
  void initState() {
    super.initState();
    _fileId = _initialFileId();
  }

  @override
  void didUpdateWidget(covariant MainWindowQualityDeck oldWidget) {
    super.didUpdateWidget(oldWidget);
    final tracks = widget.model.media.tracks;
    if (_fileId == null || !tracks.any((entry) => entry.fileId == _fileId)) {
      _fileId = _initialFileId();
      _report = null;
      _selectedSample = null;
      _error = null;
      _notice = null;
    }
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
    setState(() {
      _loading = true;
      _error = null;
      _notice = null;
      _selectedSample = null;
    });
    try {
      final report = await widget.model.deck.qualityDataSource.analyze(
        AnalysisQualityRequest(
          videoPath: track.path,
          sampleIntervalUs: 1000000,
          maxSamples: 600,
        ),
      );
      if (!mounted || serial != _requestSerial) return;
      setState(() {
        _report = report;
        _threshold = _distribution(report, _metric).p95.clamp(0.0, 1.0);
        _loading = false;
      });
    } catch (error) {
      if (!mounted || serial != _requestSerial) return;
      setState(() {
        _report = null;
        _loading = false;
        _error = error.toString();
      });
    }
  }

  void _selectMetric(AnalysisQualityMetric metric) {
    final report = _report;
    setState(() {
      _metric = metric;
      _selectedSample = null;
      _notice = null;
      if (report != null) {
        _threshold = _distribution(report, metric).p95.clamp(0.0, 1.0);
      }
    });
  }

  Future<void> _createMarks() async {
    final report = _report;
    final fileId = _fileId;
    final create = widget.actions.deck.onQualityMarksRequested;
    if (report == null || fileId == null || create == null) return;
    final created = await create(
      MainWindowQualityMarkRequest(
        fileId: fileId,
        metric: _metric,
        threshold: _threshold,
        report: report,
      ),
    );
    if (!mounted) return;
    setState(() {
      _notice = created == 0
          ? 'No new marks (matching samples may already be marked).'
          : 'Created $created metric ${created == 1 ? "mark" : "marks"}.';
    });
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final tracks = widget.model.media.tracks;
    final report = _report;
    final values = report == null
        ? const <AnalysisQualitySample>[]
        : _samplesWithMetric(report, _metric);
    final aboveThreshold = values
        .where((sample) => sample.valueFor(_metric)! >= _threshold)
        .length;
    final distribution = report == null ? null : _distribution(report, _metric);
    final sliderMax = distribution == null
        ? 1.0
        : math.max(0.05, distribution.maximum * 1.2).clamp(0.05, 1.0);

    return DecoratedBox(
      decoration: BoxDecoration(color: theme.colorScheme.surface),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(10, 7, 10, 8),
        child: Column(
          children: [
            SizedBox(
              height: 36,
              child: Row(
                children: [
                  SizedBox(
                    width: 210,
                    child: Row(
                      children: [
                        Text('Track', style: theme.textTheme.labelMedium),
                        const SizedBox(width: 8),
                        Expanded(
                          child: DropdownButton<int>(
                            value: _fileId,
                            isExpanded: true,
                            isDense: true,
                            items: [
                              for (final track in tracks)
                                DropdownMenuItem(
                                  value: track.fileId,
                                  child: Text(
                                    track.fileName,
                                    overflow: TextOverflow.ellipsis,
                                  ),
                                ),
                            ],
                            onChanged: _loading
                                ? null
                                : (fileId) {
                                    setState(() {
                                      _fileId = fileId;
                                      _report = null;
                                      _selectedSample = null;
                                      _error = null;
                                      _notice = null;
                                    });
                                  },
                          ),
                        ),
                      ],
                    ),
                  ),
                  const SizedBox(width: 8),
                  FilledButton.icon(
                    key: mainWindowQualityAnalyzeButtonKey,
                    onPressed: tracks.isEmpty || _loading ? null : _analyze,
                    icon: _loading
                        ? const SizedBox.square(
                            dimension: 14,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          )
                        : const Icon(Icons.query_stats, size: 17),
                    label: Text(_loading ? 'Analyzing…' : 'Analyze'),
                  ),
                  const SizedBox(width: 10),
                  if (report != null)
                    Expanded(
                      child: Text(
                        '${report.videoWidth}×${report.videoHeight} · '
                        '${report.samples.length} samples'
                        '${report.truncated ? " · capped" : ""}',
                        overflow: TextOverflow.ellipsis,
                        style: theme.textTheme.bodySmall?.copyWith(
                          color: theme.colorScheme.onSurfaceVariant,
                        ),
                      ),
                    ),
                ],
              ),
            ),
            const SizedBox(height: 5),
            SizedBox(
              height: 31,
              child: Row(
                children: [
                  Expanded(
                    child: ListView(
                      scrollDirection: Axis.horizontal,
                      children: [
                        for (final metric in AnalysisQualityMetric.values)
                          Padding(
                            padding: const EdgeInsets.only(right: 5),
                            child: ChoiceChip(
                              key: ValueKey(
                                'main-window-quality-metric-${metric.name}',
                              ),
                              label: Text(_metricLabel(metric)),
                              selected: metric == _metric,
                              onSelected: (_) => _selectMetric(metric),
                              visualDensity: VisualDensity.compact,
                              materialTapTargetSize:
                                  MaterialTapTargetSize.shrinkWrap,
                              labelPadding: const EdgeInsets.symmetric(
                                horizontal: 3,
                              ),
                            ),
                          ),
                      ],
                    ),
                  ),
                  if (distribution != null) ...[
                    const SizedBox(width: 8),
                    SizedBox(
                      width: 170,
                      child: Text(
                        'mean ${distribution.mean.toStringAsFixed(3)}  ·  '
                        'p95 ${distribution.p95.toStringAsFixed(3)}',
                        textAlign: TextAlign.end,
                        overflow: TextOverflow.ellipsis,
                        style: theme.textTheme.labelSmall?.copyWith(
                          color: theme.colorScheme.onSurfaceVariant,
                        ),
                      ),
                    ),
                  ],
                ],
              ),
            ),
            const SizedBox(height: 3),
            Expanded(
              child: _buildChartArea(
                context,
                values: values,
                error: _error,
                loading: _loading,
              ),
            ),
            if (report != null) ...[
              const SizedBox(height: 3),
              SizedBox(
                height: 34,
                child: Row(
                  children: [
                    Text(
                      'Threshold ${_threshold.toStringAsFixed(3)}',
                      style: theme.textTheme.labelMedium,
                    ),
                    Expanded(
                      child: Slider(
                        value: _threshold.clamp(0.0, sliderMax),
                        min: 0,
                        max: sliderMax,
                        divisions: 100,
                        onChanged: (value) {
                          setState(() {
                            _threshold = value;
                            _notice = null;
                          });
                        },
                      ),
                    ),
                    Tooltip(
                      message:
                          'Experimental proxy metric. The default threshold '
                          'is this report’s p95, not a calibrated quality limit.',
                      child: Icon(
                        Icons.science_outlined,
                        size: 18,
                        color: theme.colorScheme.onSurfaceVariant,
                      ),
                    ),
                    const SizedBox(width: 8),
                    FilledButton.tonalIcon(
                      key: mainWindowQualityCreateMarksButtonKey,
                      onPressed:
                          aboveThreshold == 0 ||
                              widget.actions.deck.onQualityMarksRequested ==
                                  null
                          ? null
                          : _createMarks,
                      icon: const Icon(
                        Icons.add_location_alt_outlined,
                        size: 17,
                      ),
                      label: Text(
                        'Create $aboveThreshold ${aboveThreshold == 1 ? "mark" : "marks"}',
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _buildChartArea(
    BuildContext context, {
    required List<AnalysisQualitySample> values,
    required String? error,
    required bool loading,
  }) {
    final theme = Theme.of(context);
    if (loading) {
      return const Center(child: Text('Decoding sampled frames…'));
    }
    if (error != null) {
      return Center(
        child: Text(
          error,
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
          'Run analysis to inspect experimental no-reference quality proxies.',
          style: theme.textTheme.bodySmall?.copyWith(
            color: theme.colorScheme.onSurfaceVariant,
          ),
        ),
      );
    }
    if (values.isEmpty) {
      return const Center(child: Text('No samples available for this metric.'));
    }

    final offsetUs = _fileId == null
        ? 0
        : widget.model.media.syncOffsets[_fileId] ?? 0;
    final playbackPtsUs = widget.model.playback.currentPtsUs - offsetUs;
    return LayoutBuilder(
      builder: (context, constraints) {
        return GestureDetector(
          key: mainWindowQualityChartKey,
          behavior: HitTestBehavior.opaque,
          onTapDown: (details) {
            final sample = _nearestSample(
              values,
              details.localPosition.dx,
              constraints.maxWidth,
            );
            setState(() => _selectedSample = sample);
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
              samples: values,
              metric: _metric,
              threshold: _threshold,
              playbackPtsUs: playbackPtsUs,
              selectedSample: _selectedSample,
              colors: theme.colorScheme,
            ),
            child: Align(
              alignment: Alignment.topRight,
              child: Padding(
                padding: const EdgeInsets.only(top: 3, right: 5),
                child: Text(
                  _notice ??
                      (_selectedSample == null
                          ? 'Click curve to seek'
                          : '${_formatTimestamp(_selectedSample!.ptsUs)} · '
                                '${_selectedSample!.valueFor(_metric)!.toStringAsFixed(3)}'),
                  style: theme.textTheme.labelSmall?.copyWith(
                    color: theme.colorScheme.onSurfaceVariant,
                  ),
                ),
              ),
            ),
          ),
        );
      },
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

List<AnalysisQualitySample> _samplesWithMetric(
  AnalysisQualityReport report,
  AnalysisQualityMetric metric,
) {
  return [
    for (final sample in report.samples)
      if (sample.valueFor(metric) != null) sample,
  ];
}

AnalysisQualitySample _nearestSample(
  List<AnalysisQualitySample> samples,
  double x,
  double width,
) {
  if (samples.length == 1 || width <= 0) return samples.first;
  final fraction = (x / width).clamp(0.0, 1.0);
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

String _metricLabel(AnalysisQualityMetric metric) => switch (metric) {
  AnalysisQualityMetric.blockiness => 'Blocking',
  AnalysisQualityMetric.banding => 'Banding',
  AnalysisQualityMetric.blur => 'Blur',
  AnalysisQualityMetric.noise => 'Noise',
  AnalysisQualityMetric.flicker => 'Flicker',
};

String _formatTimestamp(int ptsUs) {
  final totalMs = (ptsUs / 1000).round().clamp(0, 1 << 52);
  final minutes = totalMs ~/ 60000;
  final seconds = (totalMs % 60000) ~/ 1000;
  final millis = totalMs % 1000;
  return '$minutes:${seconds.toString().padLeft(2, '0')}.'
      '${millis.toString().padLeft(3, '0')}';
}

class _QualityChartPainter extends CustomPainter {
  final List<AnalysisQualitySample> samples;
  final AnalysisQualityMetric metric;
  final double threshold;
  final int playbackPtsUs;
  final AnalysisQualitySample? selectedSample;
  final ColorScheme colors;

  const _QualityChartPainter({
    required this.samples,
    required this.metric,
    required this.threshold,
    required this.playbackPtsUs,
    required this.selectedSample,
    required this.colors,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (samples.isEmpty || size.isEmpty) return;
    const top = 5.0;
    const bottom = 5.0;
    final chartHeight = math.max(1.0, size.height - top - bottom);
    final maxValue = math.max(
      0.05,
      math.max(
            threshold,
            samples
                .map((sample) => sample.valueFor(metric) ?? 0)
                .reduce(math.max),
          ) *
          1.15,
    );
    final firstPts = samples.first.ptsUs;
    final lastPts = samples.last.ptsUs;
    final duration = math.max(1, lastPts - firstPts);
    double xFor(int ptsUs) =>
        ((ptsUs - firstPts) / duration).clamp(0.0, 1.0) * size.width;
    double yFor(double value) =>
        top + chartHeight * (1 - (value / maxValue).clamp(0.0, 1.0));

    final gridPaint = Paint()
      ..color = colors.outlineVariant.withValues(alpha: 0.35)
      ..strokeWidth = 1;
    for (var line = 0; line <= 3; line++) {
      final y = top + chartHeight * line / 3;
      canvas.drawLine(Offset(0, y), Offset(size.width, y), gridPaint);
    }

    final thresholdY = yFor(threshold);
    final thresholdPaint = Paint()
      ..color = colors.tertiary
      ..strokeWidth = 1;
    for (double x = 0; x < size.width; x += 8) {
      canvas.drawLine(
        Offset(x, thresholdY),
        Offset(math.min(x + 4, size.width), thresholdY),
        thresholdPaint,
      );
    }

    final path = Path();
    for (var index = 0; index < samples.length; index++) {
      final sample = samples[index];
      final point = Offset(
        xFor(sample.ptsUs),
        yFor(sample.valueFor(metric) ?? 0),
      );
      if (index == 0) {
        path.moveTo(point.dx, point.dy);
      } else {
        path.lineTo(point.dx, point.dy);
      }
    }
    canvas.drawPath(
      path,
      Paint()
        ..color = colors.primary
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2
        ..strokeJoin = StrokeJoin.round,
    );

    for (final sample in samples) {
      final value = sample.valueFor(metric) ?? 0;
      canvas.drawCircle(
        Offset(xFor(sample.ptsUs), yFor(value)),
        identical(sample, selectedSample) ? 4 : 2.5,
        Paint()..color = value >= threshold ? colors.tertiary : colors.primary,
      );
    }

    if (playbackPtsUs >= firstPts && playbackPtsUs <= lastPts) {
      final playbackX = xFor(playbackPtsUs);
      canvas.drawLine(
        Offset(playbackX, top),
        Offset(playbackX, top + chartHeight),
        Paint()
          ..color = colors.onSurface
          ..strokeWidth = 1.2,
      );
    }
  }

  @override
  bool shouldRepaint(covariant _QualityChartPainter oldDelegate) {
    return oldDelegate.samples != samples ||
        oldDelegate.metric != metric ||
        oldDelegate.threshold != threshold ||
        oldDelegate.playbackPtsUs != playbackPtsUs ||
        oldDelegate.selectedSample != selectedSample ||
        oldDelegate.colors != colors;
  }
}

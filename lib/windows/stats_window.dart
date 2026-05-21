import 'dart:async';
import 'dart:ffi';

import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';

// ---- FFI bindings ----

final class NakiVrTrackStats extends Struct {
  @Int32()
  external int slot;
  @Int32()
  external int fileId;
  @Double()
  external double fps;
  @Double()
  external double avgDecodeMs;
  @Double()
  external double maxDecodeMs;
  @Int32()
  external int bufferCount;
  @Int32()
  external int bufferCapacity;
  @Int32()
  external int bufferState;
  @Uint64()
  external int cpuFrameMemoryBytes;
  @Uint64()
  external int packetQueueMemoryBytes;
  @Int64()
  external int currentPtsUs;
  @Int64()
  external int currentDtsUs;
}

final class NakiVrDiagnostics extends Struct {
  @Double()
  external double playbackTimeS;
  @Int32()
  external int isPlaying;
  @Int32()
  external int trackCount;
  @Uint64()
  external int processWorkingSetBytes;
  @Uint64()
  external int processPrivateBytes;
  @Uint64()
  external int dedicatedVideoMemoryBytes;
  @Uint64()
  external int cpuFrameMemoryBytes;
  @Uint64()
  external int packetQueueMemoryBytes;

  @Array(4)
  external Array<NakiVrTrackStats> tracks;

  @Int32()
  external int d3dDeviceLost;
  @Int32()
  external int reserved0;
  @Int64()
  external int d3dDeviceRemovedReason;
}

typedef _GetDiagNative = Pointer<NakiVrDiagnostics> Function();
typedef _GetDiagDart = Pointer<NakiVrDiagnostics> Function();

final _getDiag = DynamicLibrary.executable()
    .lookupFunction<_GetDiagNative, _GetDiagDart>('naki_vr_get_diagnostics');

// ---- UI panel ----

class StatsPage extends StatefulWidget {
  const StatsPage({super.key});

  @override
  State<StatsPage> createState() => _StatsPageState();
}

class _StatsPageState extends State<StatsPage> {
  List<_TrackRow> _tracks = [];
  _MemorySummary _memory = const _MemorySummary();
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    _timer = Timer.periodic(const Duration(milliseconds: 500), (_) => _poll());
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  void _poll() {
    final ptr = _getDiag();
    if (ptr == nullptr) return;
    final d = ptr.ref;
    final count = d.trackCount;
    final memory = _MemorySummary(
      workingSetBytes: d.processWorkingSetBytes,
      privateBytes: d.processPrivateBytes,
      dedicatedGpuBytes: d.dedicatedVideoMemoryBytes,
      cpuFrameBytes: d.cpuFrameMemoryBytes,
      packetQueueBytes: d.packetQueueMemoryBytes,
    );
    final list = <_TrackRow>[];
    for (int i = 0; i < count && i < 4; i++) {
      final t = d.tracks[i];
      if (t.slot < 0) continue;
      list.add(
        _TrackRow(
          fileId: t.fileId,
          fps: t.fps,
          avgDecodeMs: t.avgDecodeMs,
          maxDecodeMs: t.maxDecodeMs,
          bufferCount: t.bufferCount,
          bufferCapacity: t.bufferCapacity,
          bufferState: t.bufferState,
          cpuFrameMemoryBytes: t.cpuFrameMemoryBytes,
          packetQueueMemoryBytes: t.packetQueueMemoryBytes,
          currentPtsUs: t.currentPtsUs,
          currentDtsUs: t.currentDtsUs,
        ),
      );
    }
    if (!mounted) return;
    if (_memory == memory && _tracksEqual(_tracks, list)) return;
    setState(() {
      _memory = memory;
      _tracks = list;
    });
  }

  static bool _tracksEqual(List<_TrackRow> a, List<_TrackRow> b) {
    if (a.length != b.length) return false;
    for (int i = 0; i < a.length; i++) {
      final x = a[i], y = b[i];
      if (x.fileId != y.fileId ||
          (x.fps - y.fps).abs() > 0.05 ||
          (x.avgDecodeMs - y.avgDecodeMs).abs() > 0.01 ||
          (x.maxDecodeMs - y.maxDecodeMs).abs() > 0.01 ||
          x.bufferCount != y.bufferCount ||
          x.bufferCapacity != y.bufferCapacity ||
          x.bufferState != y.bufferState ||
          x.cpuFrameMemoryBytes != y.cpuFrameMemoryBytes ||
          x.packetQueueMemoryBytes != y.packetQueueMemoryBytes ||
          x.currentPtsUs != y.currentPtsUs ||
          x.currentDtsUs != y.currentDtsUs) {
        return false;
      }
    }
    return true;
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        _MemorySummarySection(memory: _memory),
        const Divider(height: 1),
        if (_tracks.isEmpty)
          Container(
            width: double.infinity,
            constraints: const BoxConstraints(minHeight: 72),
            alignment: Alignment.center,
            child: Text(
              l.waitingDiagnostics,
              textAlign: TextAlign.center,
              style: theme.textTheme.bodyMedium?.copyWith(
                color: theme.colorScheme.onSurfaceVariant,
              ),
            ),
          )
        else
          SingleChildScrollView(
            scrollDirection: Axis.horizontal,
            child: DataTable(
              horizontalMargin: 12,
              columnSpacing: 30,
              headingRowHeight: 34,
              dataRowMinHeight: 38,
              dataRowMaxHeight: 38,
              headingTextStyle: theme.textTheme.labelSmall,
              dataTextStyle: theme.textTheme.bodySmall,
              columns: [
                DataColumn(label: Text(l.track)),
                DataColumn(label: Text(l.fps)),
                DataColumn(label: Text(l.bufferQueue)),
                DataColumn(label: Text('Frame memory')),
                DataColumn(label: Text('Packet queue')),
                DataColumn(label: Text(l.decodeAvg)),
                DataColumn(label: Text(l.decodeMax)),
                DataColumn(label: Text(l.ptsUs)),
                DataColumn(label: Text(l.dtsUs)),
                DataColumn(label: Text(l.status)),
              ],
              rows: _tracks
                  .map(
                    (t) => DataRow(
                      cells: [
                        DataCell(Text('${t.fileId}')),
                        DataCell(Text(t.fps.toStringAsFixed(1))),
                        DataCell(Text('${t.bufferCount}/${t.bufferCapacity}')),
                        DataCell(Text(_bytesText(t.cpuFrameMemoryBytes))),
                        DataCell(Text(_bytesText(t.packetQueueMemoryBytes))),
                        DataCell(Text('${t.avgDecodeMs.toStringAsFixed(1)}ms')),
                        DataCell(Text('${t.maxDecodeMs.toStringAsFixed(1)}ms')),
                        DataCell(Text(_timestampText(l, t.currentPtsUs))),
                        DataCell(Text(_timestampText(l, t.currentDtsUs))),
                        DataCell(
                          Text(
                            t.bufferState == 1 ? l.bottleneck : l.ok,
                            style: TextStyle(
                              color: t.bufferState == 1
                                  ? Colors.orange
                                  : Colors.green,
                            ),
                          ),
                        ),
                      ],
                    ),
                  )
                  .toList(),
            ),
          ),
      ],
    );
  }
}

const int _noTimestampUs = -9223372036854775808;

String _timestampText(AppLocalizations l, int value) {
  return value == _noTimestampUs ? l.notAvailable : '$value';
}

String _bytesText(int bytes) {
  if (bytes <= 0) return '0 MB';
  final mb = bytes / 1024.0 / 1024.0;
  if (mb < 1024) return '${mb.toStringAsFixed(1)} MB';
  return '${(mb / 1024.0).toStringAsFixed(2)} GB';
}

class _MemorySummarySection extends StatelessWidget {
  static const _minTableWidth = 520.0;
  static const _cellWidth = 104.0;

  final _MemorySummary memory;

  const _MemorySummarySection({required this.memory});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    final metrics = [
      _MemoryMetric(
        label: 'RSS',
        value: _bytesText(memory.workingSetBytes),
        icon: Icons.memory_outlined,
      ),
      _MemoryMetric(
        label: 'Private',
        value: _bytesText(memory.privateBytes),
        icon: Icons.lock_outline,
      ),
      _MemoryMetric(
        label: 'GPU',
        value: _bytesText(memory.dedicatedGpuBytes),
        icon: Icons.developer_board_outlined,
      ),
      _MemoryMetric(
        label: 'CPU frames',
        value: _bytesText(memory.cpuFrameBytes),
        icon: Icons.view_in_ar_outlined,
      ),
      _MemoryMetric(
        label: 'Packets',
        value: _bytesText(memory.packetQueueBytes),
        icon: Icons.all_inbox_outlined,
      ),
    ];

    return ColoredBox(
      color: colorScheme.surfaceContainerHighest.withValues(alpha: 0.18),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  Icons.monitor_heart_outlined,
                  size: 15,
                  color: colorScheme.primary,
                ),
                const SizedBox(width: 6),
                Text(
                  'Memory',
                  style: theme.textTheme.labelMedium?.copyWith(
                    color: colorScheme.onSurfaceVariant,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 8),
            LayoutBuilder(
              builder: (context, constraints) {
                final tableWidth = constraints.maxWidth.isFinite
                    ? (constraints.maxWidth < _minTableWidth
                          ? _minTableWidth
                          : constraints.maxWidth)
                    : _minTableWidth;
                return SingleChildScrollView(
                  scrollDirection: Axis.horizontal,
                  child: SizedBox(
                    width: tableWidth,
                    child: DecoratedBox(
                      decoration: BoxDecoration(
                        border: Border.all(
                          color: colorScheme.outlineVariant.withValues(
                            alpha: 0.62,
                          ),
                        ),
                        borderRadius: BorderRadius.circular(6),
                      ),
                      child: Row(
                        children: [
                          for (int i = 0; i < metrics.length; i++) ...[
                            Expanded(
                              child: _MemoryMetricCell(metric: metrics[i]),
                            ),
                            if (i != metrics.length - 1)
                              SizedBox(
                                width: 1,
                                height: 56,
                                child: ColoredBox(
                                  color: colorScheme.outlineVariant.withValues(
                                    alpha: 0.62,
                                  ),
                                ),
                              ),
                          ],
                        ],
                      ),
                    ),
                  ),
                );
              },
            ),
          ],
        ),
      ),
    );
  }
}

class _MemoryMetricCell extends StatelessWidget {
  final _MemoryMetric metric;

  const _MemoryMetricCell({required this.metric});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    return SizedBox(
      width: _MemorySummarySection._cellWidth,
      height: 56,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 7),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Row(
              children: [
                Icon(
                  metric.icon,
                  size: 13,
                  color: colorScheme.onSurfaceVariant,
                ),
                const SizedBox(width: 5),
                Expanded(
                  child: Text(
                    metric.label,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: theme.textTheme.labelSmall?.copyWith(
                      color: colorScheme.onSurfaceVariant,
                    ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 3),
            Text(
              metric.value,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: theme.textTheme.labelLarge?.copyWith(
                fontFeatures: const [FontFeature.tabularFigures()],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _MemoryMetric {
  final String label;
  final String value;
  final IconData icon;

  const _MemoryMetric({
    required this.label,
    required this.value,
    required this.icon,
  });
}

class _MemorySummary {
  final int workingSetBytes;
  final int privateBytes;
  final int dedicatedGpuBytes;
  final int cpuFrameBytes;
  final int packetQueueBytes;

  const _MemorySummary({
    this.workingSetBytes = 0,
    this.privateBytes = 0,
    this.dedicatedGpuBytes = 0,
    this.cpuFrameBytes = 0,
    this.packetQueueBytes = 0,
  });

  @override
  bool operator ==(Object other) =>
      other is _MemorySummary &&
      workingSetBytes == other.workingSetBytes &&
      privateBytes == other.privateBytes &&
      dedicatedGpuBytes == other.dedicatedGpuBytes &&
      cpuFrameBytes == other.cpuFrameBytes &&
      packetQueueBytes == other.packetQueueBytes;

  @override
  int get hashCode => Object.hash(
    workingSetBytes,
    privateBytes,
    dedicatedGpuBytes,
    cpuFrameBytes,
    packetQueueBytes,
  );
}

class _TrackRow {
  final int fileId;
  final double fps;
  final double avgDecodeMs;
  final double maxDecodeMs;
  final int bufferCount;
  final int bufferCapacity;
  final int bufferState;
  final int cpuFrameMemoryBytes;
  final int packetQueueMemoryBytes;
  final int currentPtsUs;
  final int currentDtsUs;
  _TrackRow({
    required this.fileId,
    required this.fps,
    required this.avgDecodeMs,
    required this.maxDecodeMs,
    required this.bufferCount,
    required this.bufferCapacity,
    required this.bufferState,
    required this.cpuFrameMemoryBytes,
    required this.packetQueueMemoryBytes,
    required this.currentPtsUs,
    required this.currentDtsUs,
  });
}

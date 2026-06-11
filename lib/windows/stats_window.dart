import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'package:flutter/material.dart';

import '../l10n/app_localizations.dart';
import '../native_player/native_player_api.dart';
import '../performance/performance_health.dart';
import '../utils/async_guard.dart';

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

abstract interface class StatsDataSource {
  Future<StatsSnapshot?> load({PerformanceHealthSnapshot? previousHealth});

  static StatsDataSource forCurrentPlatform() {
    if (Platform.isWindows) {
      return WindowsFfiStatsDataSource();
    }
    return NativeDiagnosticsStatsDataSource();
  }
}

class WindowsFfiStatsDataSource implements StatsDataSource {
  late final _GetDiagDart _getDiag = DynamicLibrary.executable()
      .lookupFunction<_GetDiagNative, _GetDiagDart>('naki_vr_get_diagnostics');

  @override
  Future<StatsSnapshot?> load({
    PerformanceHealthSnapshot? previousHealth,
  }) async {
    final ptr = _getDiag();
    if (ptr == nullptr) return null;
    final d = ptr.ref;
    final memory = StatsMemorySummary(
      workingSetBytes: d.processWorkingSetBytes,
      privateBytes: d.processPrivateBytes,
      dedicatedGpuBytes: d.dedicatedVideoMemoryBytes,
      cpuFrameBytes: d.cpuFrameMemoryBytes,
      packetQueueBytes: d.packetQueueMemoryBytes,
    );
    final list = <StatsTrackRow>[];
    for (int i = 0; i < d.trackCount && i < 4; i++) {
      final t = d.tracks[i];
      if (t.slot < 0) continue;
      list.add(
        StatsTrackRow(
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
    return StatsSnapshot(
      health: PerformanceHealthSnapshot.ok(trackCount: d.trackCount),
      memory: memory,
      tracks: list,
    );
  }
}

class NativeDiagnosticsStatsDataSource implements StatsDataSource {
  final NativePlayerApi api;

  const NativeDiagnosticsStatsDataSource([
    this.api = const MethodChannelNativePlayerApi(),
  ]);

  @override
  Future<StatsSnapshot?> load({
    PerformanceHealthSnapshot? previousHealth,
  }) async {
    final diagnostics = await api.getDiagnostics();
    final tracksValue =
        diagnostics['nativeTrackDiagnostics'] ?? diagnostics['tracks'];
    final tracks = tracksValue is List ? tracksValue : const <Object?>[];
    final health = PerformanceHealthSnapshot.fromDiagnostics(
      diagnostics,
      previous: previousHealth,
    );
    return StatsSnapshot(
      health: health,
      memory: StatsMemorySummary(
        workingSetBytes: _intValue(
          diagnostics['processRssBytes'] ??
              diagnostics['processWorkingSetBytes'],
        ),
        privateBytes: _intValue(diagnostics['processPrivateBytes']),
        dedicatedGpuBytes: _intValue(
          diagnostics['dedicatedGpuUsageBytes'] ??
              diagnostics['dedicatedVideoMemoryBytes'],
        ),
        cpuFrameBytes: _intValue(
          diagnostics['cpuFrameMemoryBytes'] ??
              diagnostics['nativeCpuFrameMemoryBytes'],
        ),
        packetQueueBytes: _intValue(
          diagnostics['packetQueueMemoryBytes'] ??
              diagnostics['nativePacketQueueMemoryBytes'],
        ),
      ),
      tracks: tracks
          .whereType<Map<dynamic, dynamic>>()
          .map(
            (track) => StatsTrackRow.fromDiagnostics(
              track,
              fallbackFps: health.presentedFrameRateHz,
            ),
          )
          .toList(),
    );
  }

  static int _intValue(Object? value) {
    if (value is int) return value;
    if (value is double) return value.toInt();
    if (value is num) return value.toInt();
    return 0;
  }
}

@visibleForTesting
class StatsSnapshot {
  final PerformanceHealthSnapshot health;
  final StatsMemorySummary memory;
  final List<StatsTrackRow> tracks;

  const StatsSnapshot({
    required this.health,
    required this.memory,
    required this.tracks,
  });
}

// ---- UI panel ----

class StatsPage extends StatefulWidget {
  final StatsDataSource? dataSource;

  const StatsPage({super.key, this.dataSource});

  @override
  State<StatsPage> createState() => _StatsPageState();
}

class _StatsPageState extends State<StatsPage> {
  List<StatsTrackRow> _tracks = [];
  StatsMemorySummary _memory = const StatsMemorySummary();
  PerformanceHealthSnapshot _health = PerformanceHealthSnapshot.ok();
  late final StatsDataSource _dataSource;
  Timer? _timer;
  Future<void>? _pollInFlight;

  @override
  void initState() {
    super.initState();
    _dataSource = widget.dataSource ?? StatsDataSource.forCurrentPlatform();
    _timer = Timer.periodic(
      const Duration(milliseconds: 500),
      (_) => fireAndLogFine('poll stats panel', _poll()),
    );
    fireAndLogFine('poll stats panel', _poll());
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  Future<void> _poll() {
    final existing = _pollInFlight;
    if (existing != null) return existing;
    final next = _pollImpl().whenComplete(() {
      _pollInFlight = null;
    });
    _pollInFlight = next;
    return next;
  }

  Future<void> _pollImpl() async {
    final snapshot = await _dataSource.load(previousHealth: _health);
    if (snapshot == null) return;
    final health = snapshot.health;
    final memory = snapshot.memory;
    final list = snapshot.tracks;
    if (!mounted) return;
    if (_health == health && _memory == memory && _tracksEqual(_tracks, list)) {
      return;
    }
    setState(() {
      _health = health;
      _memory = memory;
      _tracks = list;
    });
  }

  static bool _tracksEqual(List<StatsTrackRow> a, List<StatsTrackRow> b) {
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
        StatsHealthSummarySection(health: _health),
        const Divider(height: 1),
        StatsMemorySummarySection(memory: _memory),
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
                DataColumn(label: Text(l.frameMemory)),
                DataColumn(label: Text(l.packetQueue)),
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

class StatsHealthSummarySection extends StatelessWidget {
  final PerformanceHealthSnapshot health;

  const StatsHealthSummarySection({super.key, required this.health});

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    final accent = switch (health.level) {
      PerformanceHealthLevel.ok => Colors.green,
      PerformanceHealthLevel.warning => Colors.orange,
      PerformanceHealthLevel.severe => colorScheme.error,
    };
    final icon = switch (health.level) {
      PerformanceHealthLevel.ok => Icons.check_circle_outline,
      PerformanceHealthLevel.warning => Icons.warning_amber_rounded,
      PerformanceHealthLevel.severe => Icons.error_outline,
    };
    return ColoredBox(
      color: accent.withValues(alpha: 0.08),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Icon(icon, size: 16, color: accent),
            const SizedBox(width: 8),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Expanded(
                        child: Text(
                          health.localizedTitle(context),
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: theme.textTheme.labelMedium?.copyWith(
                            color: colorScheme.onSurface,
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                      ),
                      if (health.trackCount > 0)
                        Text(
                          l.trackCount(health.trackCount),
                          style: theme.textTheme.labelSmall?.copyWith(
                            color: colorScheme.onSurfaceVariant,
                          ),
                        ),
                    ],
                  ),
                  const SizedBox(height: 4),
                  Text(
                    health.localizedDetail(context),
                    maxLines: 2,
                    overflow: TextOverflow.ellipsis,
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: colorScheme.onSurfaceVariant,
                      fontFeatures: const [FontFeature.tabularFigures()],
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class StatsMemorySummarySection extends StatelessWidget {
  final StatsMemorySummary memory;

  const StatsMemorySummarySection({super.key, required this.memory});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    final l = AppLocalizations.of(context)!;
    final metrics = [
      StatsMemoryMetric(
        label: l.memoryRss,
        value: _bytesText(memory.workingSetBytes),
        icon: Icons.memory_outlined,
      ),
      StatsMemoryMetric(
        label: l.memoryPrivate,
        value: _bytesText(memory.privateBytes),
        icon: Icons.lock_outline,
      ),
      StatsMemoryMetric(
        label: l.memoryGpuFrames,
        value: _bytesText(memory.dedicatedGpuBytes),
        icon: Icons.developer_board_outlined,
      ),
      StatsMemoryMetric(
        label: l.memoryCpuFrames,
        value: _bytesText(memory.cpuFrameBytes),
        icon: Icons.view_in_ar_outlined,
      ),
      StatsMemoryMetric(
        label: l.memoryPackets,
        value: _bytesText(memory.packetQueueBytes),
        icon: Icons.all_inbox_outlined,
      ),
    ];

    return Padding(
      padding: const EdgeInsets.fromLTRB(12, 8, 12, 8),
      child: Row(
        children: [
          for (int i = 0; i < metrics.length; i++) ...[
            Expanded(child: _MemoryMetricCell(metric: metrics[i])),
            if (i != metrics.length - 1)
              SizedBox(
                width: 1,
                height: 52,
                child: ColoredBox(
                  color: colorScheme.outlineVariant.withValues(alpha: 0.56),
                ),
              ),
          ],
        ],
      ),
    );
  }
}

class _MemoryMetricCell extends StatelessWidget {
  final StatsMemoryMetric metric;

  const _MemoryMetricCell({required this.metric});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    return SizedBox(
      height: 52,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
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

class StatsMemoryMetric {
  final String label;
  final String value;
  final IconData icon;

  const StatsMemoryMetric({
    required this.label,
    required this.value,
    required this.icon,
  });
}

class StatsMemorySummary {
  final int workingSetBytes;
  final int privateBytes;
  final int dedicatedGpuBytes;
  final int cpuFrameBytes;
  final int packetQueueBytes;

  const StatsMemorySummary({
    this.workingSetBytes = 0,
    this.privateBytes = 0,
    this.dedicatedGpuBytes = 0,
    this.cpuFrameBytes = 0,
    this.packetQueueBytes = 0,
  });

  @override
  bool operator ==(Object other) =>
      other is StatsMemorySummary &&
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

class StatsTrackRow {
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
  StatsTrackRow({
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

  factory StatsTrackRow.fromDiagnostics(
    Map<dynamic, dynamic> map, {
    double fallbackFps = 0,
  }) => StatsTrackRow(
    fileId: _intValue(map['fileId']),
    fps: _presentationFps(map, fallbackFps: fallbackFps),
    avgDecodeMs: _doubleValue(map['decodeAvgMs'] ?? map['avgDecodeMs']),
    maxDecodeMs: _doubleValue(map['decodeMaxMs'] ?? map['maxDecodeMs']),
    bufferCount: _intValue(map['bufferCount']),
    bufferCapacity: _intValue(map['bufferCapacity']),
    bufferState: _intValue(map['bufferState']),
    cpuFrameMemoryBytes: _intValue(
      map['cpuFrameMemoryBytes'] ?? map['totalCpuFrameBytes'],
    ),
    packetQueueMemoryBytes: _intValue(map['packetQueueMemoryBytes']),
    currentPtsUs: _intValue(map['currentPtsUs']),
    currentDtsUs: _intValue(map['currentDtsUs'], fallback: _noTimestampUs),
  );

  static double _presentationFps(
    Map<dynamic, dynamic> map, {
    required double fallbackFps,
  }) {
    final fps = _doubleValue(
      map['presentationFps'] ??
          map['presentedFps'] ??
          map['nativeFramePresentationFps'] ??
          map['fps'],
    );
    if (fps > 0) return fps;
    if (fallbackFps > 0) return fallbackFps;
    return _doubleValue(map['decodeFps']);
  }

  static int _intValue(Object? value, {int fallback = 0}) {
    if (value is int) return value;
    if (value is double) return value.toInt();
    if (value is num) return value.toInt();
    return fallback;
  }

  static double _doubleValue(Object? value) {
    if (value is double) return value;
    if (value is num) return value.toDouble();
    return 0.0;
  }
}

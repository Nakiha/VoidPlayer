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
}

final class NakiVrDiagnostics extends Struct {
  @Double()
  external double playbackTimeS;
  @Int32()
  external int isPlaying;
  @Int32()
  external int trackCount;

  @Array(4)
  external Array<NakiVrTrackStats> tracks;
}

typedef _GetDiagNative = Pointer<NakiVrDiagnostics> Function();
typedef _GetDiagDart = Pointer<NakiVrDiagnostics> Function();

final _getDiag = DynamicLibrary.executable()
    .lookupFunction<_GetDiagNative, _GetDiagDart>('naki_vr_get_diagnostics');

// ---- UI ----

class StatsApp extends StatelessWidget {
  const StatsApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Void Player - Stats',
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF0078D4),
          brightness: Brightness.dark,
        ),
      ),
      home: const StatsPage(),
    );
  }
}

class StatsPage extends StatefulWidget {
  const StatsPage({super.key});

  @override
  State<StatsPage> createState() => _StatsPageState();
}

class _StatsPageState extends State<StatsPage> {
  List<_TrackRow> _tracks = [];
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
    final list = <_TrackRow>[];
    for (int i = 0; i < count && i < 4; i++) {
      final t = d.tracks[i];
      if (t.slot < 0) continue;
      list.add(_TrackRow(
        fileId: t.fileId,
        fps: t.fps,
        avgDecodeMs: t.avgDecodeMs,
        maxDecodeMs: t.maxDecodeMs,
        bufferCount: t.bufferCount,
        bufferCapacity: t.bufferCapacity,
        bufferState: t.bufferState,
      ));
    }
    if (!mounted) return;
    if (_tracksEqual(_tracks, list)) return;
    setState(() => _tracks = list);
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
          x.bufferState != y.bufferState) {
        return false;
      }
    }
    return true;
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    return Scaffold(
      body: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(l.trackStatistics, style: theme.textTheme.titleMedium),
            const SizedBox(height: 8),
            Expanded(
              child: _tracks.isEmpty
                  ? Center(
                      child: Text(l.waitingDiagnostics,
                          style: theme.textTheme.bodyMedium?.copyWith(
                              color: theme.colorScheme.onSurfaceVariant)))
                  : DataTable(
                      headingTextStyle: theme.textTheme.labelSmall,
                      dataTextStyle: theme.textTheme.bodySmall,
                      columns: [
                        DataColumn(label: Text(l.track)),
                        DataColumn(label: Text(l.fps)),
                        DataColumn(label: Text(l.target)),
                        DataColumn(label: Text(l.decodeAvg)),
                        DataColumn(label: Text(l.decodeMax)),
                        DataColumn(label: Text(l.status)),
                      ],
                      rows: _tracks.map((t) => DataRow(cells: [
                        DataCell(Text('${t.fileId}')),
                        DataCell(Text(t.fps.toStringAsFixed(1))),
                        DataCell(Text('${t.bufferCount}/${t.bufferCapacity}')),
                        DataCell(Text('${t.avgDecodeMs.toStringAsFixed(1)}ms')),
                        DataCell(Text('${t.maxDecodeMs.toStringAsFixed(1)}ms')),
                        DataCell(Text(
                          t.bufferState == 1 ? l.bottleneck : l.ok,
                          style: TextStyle(
                            color: t.bufferState == 1 ? Colors.orange : Colors.green,
                          ),
                        )),
                      ])).toList()),
            ),
          ],
        ),
      ),
    );
  }
}

class _TrackRow {
  final int fileId;
  final double fps;
  final double avgDecodeMs;
  final double maxDecodeMs;
  final int bufferCount;
  final int bufferCapacity;
  final int bufferState;
  _TrackRow({
    required this.fileId,
    required this.fps,
    required this.avgDecodeMs,
    required this.maxDecodeMs,
    required this.bufferCount,
    required this.bufferCapacity,
    required this.bufferState,
  });
}

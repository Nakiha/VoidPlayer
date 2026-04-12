import 'dart:async';
import 'package:flutter/material.dart';
import '../analysis/analysis_cache.dart';
import '../analysis/analysis_ffi.dart';
import '../analysis/nalu_types.dart';

// ===========================================================================
// Analysis Window — secondary Flutter window for bitstream visualization
// ===========================================================================

class AnalysisApp extends StatelessWidget {
  final Color accentColor;
  final String hash;

  const AnalysisApp({super.key, required this.accentColor, required this.hash});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Void Player - Analysis',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        brightness: Brightness.dark,
        colorSchemeSeed: accentColor,
        useMaterial3: true,
      ),
      home: AnalysisPage(hash: hash),
    );
  }
}

// ===========================================================================

class AnalysisPage extends StatefulWidget {
  final String hash;
  const AnalysisPage({super.key, required this.hash});

  @override
  State<AnalysisPage> createState() => _AnalysisPageState();
}

class _AnalysisPageState extends State<AnalysisPage> {
  int _selectedTab = 0; // 0=ref pyramid, 1=frame trend
  bool _ptsOrder = true;
  int? _selectedNaluIdx;

  List<NakiFrameInfo> _frames = [];
  List<NakiNaluInfo> _nalus = [];
  NakiAnalysisSummary? _summary;
  Timer? _pollTimer;

  @override
  void initState() {
    super.initState();
    _loadData();
    _pollTimer = Timer.periodic(
      const Duration(milliseconds: 200),
      (_) => _poll(),
    );
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    super.dispose();
  }

  void _loadData() {
    final hash = widget.hash;
    final vbs2 = AnalysisCache.vbs2Path(hash);
    final vbi = AnalysisCache.vbiPath(hash);
    final vbt = AnalysisCache.vbtPath(hash);

    // Load via FFI (analysis files may already be loaded from main window)
    final s = AnalysisFfi.summary;
    if (s.loaded == 0) {
      // Need to load — VBS2 is optional
      AnalysisFfi.load(vbs2, vbi, vbt);
    }
    _readData();
  }

  void _readData() {
    final s = AnalysisFfi.summary;
    if (s.loaded == 0) return;
    _summary = s;
    _frames = AnalysisFfi.frames;
    _nalus = AnalysisFfi.nalus;
    if (mounted) {
      setState(() {});
    }
  }

  void _poll() {
    final s = AnalysisFfi.summary;
    if (s.loaded == 0) return;
    if (_summary != null &&
        s.currentFrameIdx == _summary!.currentFrameIdx &&
        s.frameCount == _summary!.frameCount) return;
    _summary = s;
    if (mounted) {
      setState(() {});
    }
  }

  List<NakiFrameInfo> get _sortedFrames {
    final list = List<NakiFrameInfo>.from(_frames);
    if (!_ptsOrder) {
      list.sort((a, b) => a.dts.compareTo(b.dts));
    }
    return list;
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      body: Column(
        children: [
          // Top bar: order toggle + tab bar
          Container(
            height: 40,
            padding: const EdgeInsets.symmetric(horizontal: 8),
            decoration: BoxDecoration(
              border: Border(bottom: BorderSide(color: theme.dividerColor)),
            ),
            child: Row(
              children: [
                // Order toggle
                _OrderToggle(
                  ptsOrder: _ptsOrder,
                  onChanged: (v) => setState(() => _ptsOrder = v),
                ),
                const Spacer(),
                // Tab bar
                _TabBar(
                  selectedTab: _selectedTab,
                  onTabChanged: (i) => setState(() => _selectedTab = i),
                ),
              ],
            ),
          ),
          // Top panel: ref pyramid or frame trend (40%)
          Expanded(
            flex: 4,
            child: _selectedTab == 0
                ? _ReferencePyramidView(
                    frames: _sortedFrames, currentIdx: _summary?.currentFrameIdx ?? -1)
                : _FrameTrendView(
                    frames: _sortedFrames, currentIdx: _summary?.currentFrameIdx ?? -1),
          ),
          const Divider(height: 1),
          // Bottom: NALU browser + detail (60%)
          Expanded(
            flex: 6,
            child: Row(
              children: [
                // NALU browser
                Expanded(
                  flex: 7,
                  child: _NaluBrowserView(
                    nalus: _nalus,
                    selectedIdx: _selectedNaluIdx,
                    onSelected: (i) => setState(() => _selectedNaluIdx = i),
                  ),
                ),
                const VerticalDivider(width: 1),
                // NALU detail
                Expanded(
                  flex: 3,
                  child: _NaluDetailView(
                    nalu: _selectedNaluIdx != null && _selectedNaluIdx! < _nalus.length
                        ? _nalus[_selectedNaluIdx!]
                        : null,
                    frames: _frames,
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

// ===========================================================================
// Top bar widgets
// ===========================================================================

class _OrderToggle extends StatelessWidget {
  final bool ptsOrder;
  final ValueChanged<bool> onChanged;
  const _OrderToggle({required this.ptsOrder, required this.onChanged});

  @override
  Widget build(BuildContext context) {
    return SegmentedButton<bool>(
      segments: const [
        ButtonSegment(value: true, label: Text('PTS')),
        ButtonSegment(value: false, label: Text('DTS')),
      ],
      selected: {ptsOrder},
      onSelectionChanged: (s) => onChanged(s.first),
      style: ButtonStyle(
        visualDensity: VisualDensity.compact,
        textStyle: WidgetStatePropertyAll(TextStyle(fontSize: 12)),
      ),
    );
  }
}

class _TabBar extends StatelessWidget {
  final int selectedTab;
  final ValueChanged<int> onTabChanged;
  const _TabBar({required this.selectedTab, required this.onTabChanged});

  @override
  Widget build(BuildContext context) {
    return SegmentedButton<int>(
      segments: const [
        ButtonSegment(value: 0, label: Text('Ref Pyramid')),
        ButtonSegment(value: 1, label: Text('Frame Trend')),
      ],
      selected: {selectedTab},
      onSelectionChanged: (s) => onTabChanged(s.first),
      style: ButtonStyle(
        visualDensity: VisualDensity.compact,
        textStyle: WidgetStatePropertyAll(TextStyle(fontSize: 12)),
      ),
    );
  }
}

// ===========================================================================
// Reference Pyramid (placeholder — CustomPainter in next step)
// ===========================================================================

class _ReferencePyramidView extends StatelessWidget {
  final List<NakiFrameInfo> frames;
  final int currentIdx;
  const _ReferencePyramidView({required this.frames, required this.currentIdx});

  @override
  Widget build(BuildContext context) {
    if (frames.isEmpty) {
      return const Center(child: Text('No frame data'));
    }
    return CustomPaint(
      painter: _RefPyramidPainter(frames: frames, currentIdx: currentIdx),
      size: Size.infinite,
    );
  }
}

class _RefPyramidPainter extends CustomPainter {
  final List<NakiFrameInfo> frames;
  final int currentIdx;

  _RefPyramidPainter({required this.frames, required this.currentIdx});

  @override
  void paint(Canvas canvas, Size size) {
    if (frames.isEmpty) return;

    final paint = Paint()..style = PaintingStyle.fill;
    final barW = (size.width / frames.length).clamp(2.0, 20.0);
    final maxTid = 6;
    final rowH = size.height / (maxTid + 1);

    for (var i = 0; i < frames.length; i++) {
      final f = frames[i];
      final x = i * barW;
      final tid = f.temporalId.clamp(0, maxTid);

      // Color by slice type: I=red, P=blue, B=green
      paint.color = switch (f.sliceType) {
        2 => f.keyframe == 1 ? const Color(0xFFFF5252) : const Color(0xFFE53935),
        1 => const Color(0xFF42A5F5),
        _ => const Color(0xFF66BB6A),
      };

      final y = size.height - (tid + 1) * rowH;
      final h = rowH - 1;
      canvas.drawRect(Rect.fromLTWH(x, y, barW - 1, h), paint);

      // Current frame cursor
      if (i == currentIdx) {
        final cursorPaint = Paint()
          ..color = const Color(0xFFFFFFFF)
          ..strokeWidth = 1.5;
        canvas.drawLine(
          Offset(x + barW / 2, 0),
          Offset(x + barW / 2, size.height),
          cursorPaint,
        );
      }
    }
  }

  @override
  bool shouldRepaint(covariant _RefPyramidPainter old) =>
      frames.length != old.frames.length || currentIdx != old.currentIdx;
}

// ===========================================================================
// Frame Trend (placeholder — CustomPainter in next step)
// ===========================================================================

class _FrameTrendView extends StatelessWidget {
  final List<NakiFrameInfo> frames;
  final int currentIdx;
  const _FrameTrendView({required this.frames, required this.currentIdx});

  @override
  Widget build(BuildContext context) {
    if (frames.isEmpty) {
      return const Center(child: Text('No frame data'));
    }
    return CustomPaint(
      painter: _FrameTrendPainter(frames: frames, currentIdx: currentIdx),
      size: Size.infinite,
    );
  }
}

class _FrameTrendPainter extends CustomPainter {
  final List<NakiFrameInfo> frames;
  final int currentIdx;

  _FrameTrendPainter({required this.frames, required this.currentIdx});

  @override
  void paint(Canvas canvas, Size size) {
    if (frames.isEmpty) return;

    final barW = (size.width / frames.length).clamp(2.0, 20.0);
    final upperH = size.height * 0.6; // packet size
    final lowerH = size.height * 0.35; // QP
    final gap = size.height * 0.05;

    // Find max packet size for scaling
    int maxPacketSize = 1;
    for (final f in frames) {
      if (f.packetSize > maxPacketSize) maxPacketSize = f.packetSize;
    }

    final barPaint = Paint()..style = PaintingStyle.fill;
    final qpPaint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5
      ..color = const Color(0xFFFFB74D);

    // Draw packet size bars
    for (var i = 0; i < frames.length; i++) {
      final f = frames[i];
      final x = i * barW;
      final h = (f.packetSize / maxPacketSize) * upperH;

      barPaint.color = f.keyframe == 1
          ? const Color(0xFFFF5252)
          : const Color(0xFF42A5F5);

      canvas.drawRect(
        Rect.fromLTWH(x, upperH - h, barW - 1, h),
        barPaint,
      );

      // Keyframe marker
      if (f.keyframe == 1) {
        final markerPaint = Paint()..color = const Color(0xFFFFD54F);
        canvas.drawRect(
          Rect.fromLTWH(x, upperH - h - 3, barW - 1, 2),
          markerPaint,
        );
      }
    }

    // Draw QP line
    final qpPath = Path();
    for (var i = 0; i < frames.length; i++) {
      final f = frames[i];
      final x = i * barW + barW / 2;
      final y = upperH + gap + (f.avgQp / 63.0) * lowerH;
      if (i == 0) {
        qpPath.moveTo(x, y);
      } else {
        qpPath.lineTo(x, y);
      }
    }
    canvas.drawPath(qpPath, qpPaint);

    // Current frame cursor
    if (currentIdx >= 0 && currentIdx < frames.length) {
      final cx = currentIdx * barW + barW / 2;
      final cursorPaint = Paint()
        ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.7)
        ..strokeWidth = 1;
      canvas.drawLine(Offset(cx, 0), Offset(cx, size.height), cursorPaint);
    }
  }

  @override
  bool shouldRepaint(covariant _FrameTrendPainter old) =>
      frames.length != old.frames.length || currentIdx != old.currentIdx;
}

// ===========================================================================
// NALU Browser
// ===========================================================================

class _NaluBrowserView extends StatelessWidget {
  final List<NakiNaluInfo> nalus;
  final int? selectedIdx;
  final ValueChanged<int> onSelected;

  const _NaluBrowserView({
    required this.nalus,
    required this.selectedIdx,
    required this.onSelected,
  });

  @override
  Widget build(BuildContext context) {
    if (nalus.isEmpty) {
      return const Center(child: Text('No NALU data'));
    }
    return LayoutBuilder(builder: (context, constraints) {
      return CustomPaint(
        painter: _NaluBrowserPainter(
          nalus: nalus,
          selectedIdx: selectedIdx,
          width: constraints.maxWidth,
        ),
        size: Size(constraints.maxWidth, constraints.maxHeight),
      );
    });
  }
}

class _NaluBrowserPainter extends CustomPainter {
  final List<NakiNaluInfo> nalus;
  final int? selectedIdx;
  final double width;

  _NaluBrowserPainter({
    required this.nalus,
    required this.selectedIdx,
    required this.width,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (nalus.isEmpty) return;

    // Find max size for scaling
    int maxSize = 1;
    for (final n in nalus) {
      if (n.size > maxSize) maxSize = n.size;
    }

    final paint = Paint()..style = PaintingStyle.fill;
    const rowH = 20.0;
    double x = 0;
    double y = 2;
    final minBlockW = 3.0;
    final maxBlockW = 80.0;

    for (var i = 0; i < nalus.length; i++) {
      final n = nalus[i];
      // Logarithmic scale for width
      final logScale = (n.size / maxSize);
      final blockW = (minBlockW + logScale * (maxBlockW - minBlockW)).clamp(minBlockW, maxBlockW);

      if (x + blockW > width) {
        x = 0;
        y += rowH + 2;
        if (y + rowH > size.height) break;
      }

      // Color by NAL type
      paint.color = Color(naluTypeColor(n.nalType));

      final rect = Rect.fromLTWH(x, y, blockW, rowH);
      canvas.drawRect(rect, paint);

      // Selected highlight
      if (i == selectedIdx) {
        final borderPaint = Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2
          ..color = const Color(0xFFFFFFFF);
        canvas.drawRect(rect, borderPaint);
      }

      // Keyframe border
      if (n.flags & 0x04 != 0) {
        final kfPaint = Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1
          ..color = const Color(0xFFFFD54F);
        canvas.drawRect(rect.deflate(1), kfPaint);
      }

      x += blockW + 1;
    }
  }

  @override
  bool shouldRepaint(covariant _NaluBrowserPainter old) =>
      nalus.length != old.nalus.length || selectedIdx != old.selectedIdx;
}

// ===========================================================================
// NALU Detail
// ===========================================================================

class _NaluDetailView extends StatelessWidget {
  final NakiNaluInfo? nalu;
  final List<NakiFrameInfo> frames;

  const _NaluDetailView({required this.nalu, required this.frames});

  @override
  Widget build(BuildContext context) {
    if (nalu == null) {
      return const Center(child: Text('Select a NALU'));
    }
    final n = nalu!;
    final theme = Theme.of(context);
    final ts = theme.textTheme.bodySmall!;

    final items = <_DetailRow>[
      _DetailRow('Type', '${h266NaluTypeName(n.nalType)} (${n.nalType})'),
      _DetailRow('Temporal ID', '${n.temporalId}'),
      _DetailRow('Layer ID', '${n.layerId}'),
      _DetailRow('Offset', '${n.offset}'),
      _DetailRow('Size', '${n.size} bytes'),
      _DetailRow('VCL', '${(n.flags & 0x01) != 0}'),
      _DetailRow('Slice', '${(n.flags & 0x02) != 0}'),
      _DetailRow('Keyframe', '${(n.flags & 0x04) != 0}'),
    ];

    return Padding(
      padding: const EdgeInsets.all(12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('NALU Detail', style: theme.textTheme.titleSmall),
          const SizedBox(height: 8),
          ...items.map((r) => Padding(
                padding: const EdgeInsets.symmetric(vertical: 2),
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    SizedBox(
                      width: 80,
                      child: Text(r.label,
                          style: ts.copyWith(
                              color: theme.colorScheme.onSurfaceVariant)),
                    ),
                    Expanded(child: Text(r.value, style: ts)),
                  ],
                ),
              )),
        ],
      ),
    );
  }
}

class _DetailRow {
  final String label;
  final String value;
  const _DetailRow(this.label, this.value);
}

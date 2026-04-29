import 'dart:math' as math;

import 'package:flutter/material.dart';

import 'actions/action_registry.dart';
import 'l10n/app_localizations.dart';
import 'startup_options.dart';
import 'windows/main/main_window.dart';

const _dcompKeyColor = Color(0xFF00FFFF);
const _dcompProbePanelColor = Color(0xFF242428);

class VoidPlayerApp extends StatelessWidget {
  final Color accentColor;
  final String? testScriptPath;
  final StartupOptions startupOptions;
  final bool dcompAlphaProbe;
  final bool dcompSurfaceProbe;
  final bool dcompHdrSdrMixProbe;

  const VoidPlayerApp({
    super.key,
    required this.accentColor,
    this.testScriptPath,
    this.startupOptions = const StartupOptions(),
    this.dcompAlphaProbe = false,
    this.dcompSurfaceProbe = false,
    this.dcompHdrSdrMixProbe = false,
  });

  @override
  Widget build(BuildContext context) {
    final dcompProbe =
        dcompAlphaProbe || dcompSurfaceProbe || dcompHdrSdrMixProbe;
    return MaterialApp(
      title: dcompProbe ? 'Void Player DComp Probe' : 'Void Player',
      color: dcompProbe ? Colors.transparent : null,
      debugShowCheckedModeBanner: !dcompProbe,
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: accentColor,
          brightness: Brightness.light,
        ),
        scaffoldBackgroundColor: dcompProbe ? Colors.transparent : null,
      ),
      darkTheme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: accentColor,
          brightness: Brightness.dark,
        ),
        scaffoldBackgroundColor: dcompProbe ? Colors.transparent : null,
      ),
      themeMode: dcompProbe ? ThemeMode.dark : ThemeMode.system,
      home: dcompProbe
          ? _DCompAlphaHoleProbePage(
              colorKeyMode: dcompAlphaProbe || dcompHdrSdrMixProbe,
              hdrSdrMixMode: dcompHdrSdrMixProbe,
            )
          : ActionFocus(
              child: MainWindow(
                testScriptPath: testScriptPath,
                startupOptions: startupOptions,
              ),
            ),
    );
  }
}

class _DCompAlphaHoleProbePage extends StatefulWidget {
  const _DCompAlphaHoleProbePage({
    required this.colorKeyMode,
    required this.hdrSdrMixMode,
  });

  final bool colorKeyMode;
  final bool hdrSdrMixMode;

  @override
  State<_DCompAlphaHoleProbePage> createState() =>
      _DCompAlphaHoleProbePageState();
}

class _DCompAlphaHoleProbePageState extends State<_DCompAlphaHoleProbePage> {
  static const _dragSize = Size(280, 92);

  Offset? _dragOffset;

  Offset _clampDragOffset(Offset value, Size bounds) {
    return Offset(
      value.dx.clamp(0.0, math.max(0.0, bounds.width - _dragSize.width)),
      value.dy.clamp(0.0, math.max(0.0, bounds.height - _dragSize.height)),
    );
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      backgroundColor: widget.colorKeyMode
          ? _dcompProbePanelColor
          : Colors.transparent,
      body: LayoutBuilder(
        builder: (context, constraints) {
          final holeWidth = math.max(
            120.0,
            math.min(640.0, constraints.maxWidth - 96.0),
          );
          final holeHeight = math.max(
            90.0,
            math.min(360.0, constraints.maxHeight - 150.0),
          );
          final canvasSize = Size(constraints.maxWidth, constraints.maxHeight);
          final hole = Rect.fromCenter(
            center: Offset(constraints.maxWidth / 2, constraints.maxHeight / 2),
            width: holeWidth,
            height: holeHeight,
          );
          final initialDragOffset = Offset(
            hole.left + holeWidth * 0.35,
            hole.center.dy - _dragSize.height / 2,
          );
          final dragOffset =
              _dragOffset ?? _clampDragOffset(initialDragOffset, canvasSize);
          return Stack(
            children: [
              Positioned.fill(
                child: IgnorePointer(
                  child: CustomPaint(
                    painter: _HoleBackdropPainter(
                      holeWidth: holeWidth,
                      holeHeight: holeHeight,
                      colorKeyMode: widget.colorKeyMode,
                    ),
                  ),
                ),
              ),
              Positioned(
                left: math.max(24, hole.left - 170),
                top: hole.top + 18,
                child: const _ProbeChip(
                  label: 'Flutter panel',
                  color: Color(0xFF5B7CFA),
                ),
              ),
              Positioned(
                left: hole.right + 20,
                top: hole.top + 18,
                child: const _ProbeChip(
                  label: 'Native HDR',
                  color: Color(0xFF00A86B),
                ),
              ),
              Positioned(
                left: hole.left + 24,
                top: math.max(64, hole.top - 36),
                child: const _ProbeChip(
                  label: 'Top edge',
                  color: Color(0xFFB86BFF),
                ),
              ),
              Positioned(
                left: hole.left + 18,
                top: hole.bottom - 18,
                width: 180,
                height: 36,
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    color: const Color(0x99FFB000),
                    border: Border.all(color: const Color(0xFFFFD166)),
                  ),
                  child: const Center(
                    child: Text(
                      'static alpha edge',
                      style: TextStyle(fontWeight: FontWeight.w700),
                    ),
                  ),
                ),
              ),
              Align(
                alignment: Alignment.center,
                child: SizedBox(
                  width: holeWidth,
                  height: holeHeight,
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      color: Colors.transparent,
                      border: Border.all(color: Colors.white, width: 2),
                    ),
                  ),
                ),
              ),
              Positioned(
                left: dragOffset.dx,
                top: dragOffset.dy,
                width: _dragSize.width,
                height: _dragSize.height,
                child: GestureDetector(
                  behavior: HitTestBehavior.opaque,
                  onPanUpdate: (details) {
                    setState(() {
                      _dragOffset = _clampDragOffset(
                        dragOffset + details.delta,
                        canvasSize,
                      );
                    });
                  },
                  child: const _DraggableAlphaProbe(),
                ),
              ),
              Align(
                alignment: Alignment.center,
                child: Transform.translate(
                  offset: Offset(0, -(holeHeight / 2 + 40)),
                  child: Material(
                    color: Colors.black.withValues(alpha: 0.72),
                    borderRadius: BorderRadius.circular(8),
                    child: Padding(
                      padding: const EdgeInsets.symmetric(
                        horizontal: 16,
                        vertical: 10,
                      ),
                      child: Text(
                        widget.hdrSdrMixMode
                            ? 'DComp HDR/SDR mix probe'
                            : 'DComp HDR probe',
                        style: theme.textTheme.titleMedium,
                      ),
                    ),
                  ),
                ),
              ),
              Align(
                alignment: Alignment.center,
                child: Material(
                  color: const Color(0xFFFF2D55),
                  borderRadius: BorderRadius.circular(8),
                  child: const Padding(
                    padding: EdgeInsets.symmetric(horizontal: 14, vertical: 8),
                    child: Text(
                      'Flutter overlay on top',
                      style: TextStyle(
                        color: Colors.white,
                        fontWeight: FontWeight.w700,
                      ),
                    ),
                  ),
                ),
              ),
              Positioned(
                left: 24,
                right: 24,
                bottom: 24,
                child: Material(
                  color: Colors.black.withValues(alpha: 0.72),
                  borderRadius: BorderRadius.circular(8),
                  child: const Padding(
                    padding: EdgeInsets.all(12),
                    child: Text(
                      'Native DirectComposition FP16/scRGB bands: dark grid, '
                      'SDR white 1.0, HDR white 4.0, HDR green 8.0. '
                      'Pink control is Flutter UI above the native surface. '
                      'HDR/SDR mix probe adds a native SDR premultiplied '
                      'alpha overlay inside the DComp tree.',
                    ),
                  ),
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}

class _ProbeChip extends StatelessWidget {
  const _ProbeChip({required this.label, required this.color});

  final String label;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: color,
        borderRadius: BorderRadius.circular(6),
        border: Border.all(color: Colors.white.withValues(alpha: 0.54)),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 7),
        child: Text(
          label,
          style: const TextStyle(
            color: Colors.white,
            fontWeight: FontWeight.w700,
          ),
        ),
      ),
    );
  }
}

class _DraggableAlphaProbe extends StatelessWidget {
  const _DraggableAlphaProbe();

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: const Color(0x99FFB000),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: const Color(0xFFFFD166), width: 2),
        boxShadow: const [
          BoxShadow(
            color: Color(0x55000000),
            blurRadius: 18,
            offset: Offset(0, 8),
          ),
        ],
      ),
      child: const Padding(
        padding: EdgeInsets.all(12),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'Drag semi-transparent Flutter',
              style: TextStyle(
                color: Colors.white,
                fontWeight: FontWeight.w800,
              ),
            ),
            SizedBox(height: 6),
            Text(
              'Move across the HDR/key boundary',
              style: TextStyle(color: Colors.white),
            ),
          ],
        ),
      ),
    );
  }
}

class _HoleBackdropPainter extends CustomPainter {
  const _HoleBackdropPainter({
    required this.holeWidth,
    required this.holeHeight,
    required this.colorKeyMode,
  });

  final double holeWidth;
  final double holeHeight;
  final bool colorKeyMode;

  @override
  void paint(Canvas canvas, Size size) {
    final hole = Rect.fromCenter(
      center: Offset(size.width / 2, size.height / 2),
      width: holeWidth,
      height: holeHeight,
    );
    if (colorKeyMode) {
      canvas.drawRect(
        Offset.zero & size,
        Paint()..color = _dcompProbePanelColor,
      );
      canvas.drawRect(hole, Paint()..color = _dcompKeyColor);
      return;
    }

    final panelPath = Path()
      ..fillType = PathFillType.evenOdd
      ..addRect(Offset.zero & size)
      ..addRRect(RRect.fromRectAndRadius(hole, const Radius.circular(24)));
    canvas.drawPath(
      panelPath,
      Paint()
        ..color = _dcompProbePanelColor
        ..isAntiAlias = true,
    );
  }

  @override
  bool shouldRepaint(_HoleBackdropPainter oldDelegate) {
    return oldDelegate.holeWidth != holeWidth ||
        oldDelegate.holeHeight != holeHeight ||
        oldDelegate.colorKeyMode != colorKeyMode;
  }
}

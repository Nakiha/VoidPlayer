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

  const VoidPlayerApp({
    super.key,
    required this.accentColor,
    this.testScriptPath,
    this.startupOptions = const StartupOptions(),
    this.dcompAlphaProbe = false,
  });

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: dcompAlphaProbe ? 'Void Player DComp Probe' : 'Void Player',
      color: dcompAlphaProbe ? Colors.transparent : null,
      debugShowCheckedModeBanner: !dcompAlphaProbe,
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: accentColor,
          brightness: Brightness.light,
        ),
        scaffoldBackgroundColor: dcompAlphaProbe ? Colors.transparent : null,
      ),
      darkTheme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: accentColor,
          brightness: Brightness.dark,
        ),
        scaffoldBackgroundColor: dcompAlphaProbe ? Colors.transparent : null,
      ),
      themeMode: dcompAlphaProbe ? ThemeMode.dark : ThemeMode.system,
      home: dcompAlphaProbe
          ? const _DCompAlphaHoleProbePage()
          : ActionFocus(
              child: MainWindow(
                testScriptPath: testScriptPath,
                startupOptions: startupOptions,
              ),
            ),
    );
  }
}

class _DCompAlphaHoleProbePage extends StatelessWidget {
  const _DCompAlphaHoleProbePage();

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      backgroundColor: _dcompProbePanelColor,
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
          return Stack(
            children: [
              Positioned.fill(
                child: IgnorePointer(
                  child: CustomPaint(
                    painter: _HoleBackdropPainter(
                      holeWidth: holeWidth,
                      holeHeight: holeHeight,
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
                        'DComp HDR probe',
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
                      'Pink control is Flutter UI above the native surface.',
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

class _HoleBackdropPainter extends CustomPainter {
  const _HoleBackdropPainter({
    required this.holeWidth,
    required this.holeHeight,
  });

  final double holeWidth;
  final double holeHeight;

  @override
  void paint(Canvas canvas, Size size) {
    final hole = Rect.fromCenter(
      center: Offset(size.width / 2, size.height / 2),
      width: holeWidth,
      height: holeHeight,
    );
    canvas.drawRect(Offset.zero & size, Paint()..color = _dcompProbePanelColor);
    canvas.drawRect(hole, Paint()..color = _dcompKeyColor);
  }

  @override
  bool shouldRepaint(_HoleBackdropPainter oldDelegate) {
    return oldDelegate.holeWidth != holeWidth ||
        oldDelegate.holeHeight != holeHeight;
  }
}

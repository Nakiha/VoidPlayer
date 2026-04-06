import 'dart:async';
import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';

/// Memory/performance monitor window (secondary window, 800x600).
class MemoryApp extends StatelessWidget {
  const MemoryApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Void Player - Memory Monitor',
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF0078D4),
          brightness: Brightness.dark,
        ),
      ),
      home: const MemoryPage(),
    );
  }
}

class MemoryPage extends StatefulWidget {
  const MemoryPage({super.key});

  @override
  State<MemoryPage> createState() => _MemoryPageState();
}

class _MemoryPageState extends State<MemoryPage> {
  final List<double> _memoryHistory = [];
  Timer? _pollTimer;

  @override
  void initState() {
    super.initState();
    _pollTimer = Timer.periodic(
      const Duration(seconds: 1),
      (_) => _pollMemory(),
    );
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    super.dispose();
  }

  Future<void> _pollMemory() async {
    // TODO: replace with actual memory query when native layer exposes it
    if (!mounted) return;
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    return Scaffold(
      body: Padding(
        padding: const EdgeInsets.all(8),
        child: Column(
          children: [
            // Memory chart area
            Expanded(
              flex: 3,
              child: CustomPaint(
                size: Size.infinite,
                painter: _MemoryChartPainter(
                  data: _memoryHistory,
                  lineColor: theme.colorScheme.primary,
                  bgColor: theme.colorScheme.surfaceContainerLow,
                  noDataText: l.noDataYet,
                ),
              ),
            ),
            const Divider(height: 1),
            // Object stats area (placeholder)
            Expanded(
              flex: 2,
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Padding(
                    padding: const EdgeInsets.all(8),
                    child: Text(l.objectStatistics,
                        style: theme.textTheme.titleSmall),
                  ),
                  Expanded(
                    child: Center(
                      child: Text(
                        l.memoryPlaceholder,
                        style: theme.textTheme.bodyMedium?.copyWith(
                              color: theme.colorScheme.onSurfaceVariant,
                            ),
                        textAlign: TextAlign.center,
                      ),
                    ),
                  ),
                ],
              ),
            ),
            // Action buttons
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 4),
              child: Row(
                children: [
                  ElevatedButton(
                    onPressed: () {
                      // TODO: implement snapshot
                    },
                    child: Text(l.snapshot),
                  ),
                  const SizedBox(width: 8),
                  OutlinedButton(
                    onPressed: () {
                      setState(() => _memoryHistory.clear());
                    },
                    child: Text(l.clear),
                  ),
                  const Spacer(),
                  Text(
                    l.statusWaitingData,
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant,
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

/// Simple line chart painter for memory history.
class _MemoryChartPainter extends CustomPainter {
  final List<double> data;
  final Color lineColor;
  final Color bgColor;
  final String noDataText;

  _MemoryChartPainter({
    required this.data,
    required this.lineColor,
    required this.bgColor,
    required this.noDataText,
  });

  @override
  void paint(Canvas canvas, Size size) {
    // Background
    canvas.drawRect(
      Rect.fromLTWH(0, 0, size.width, size.height),
      Paint()..color = bgColor,
    );

    if (data.isEmpty) {
      final tp = TextPainter(
        text: TextSpan(text: noDataText, style: const TextStyle(color: Colors.grey)),
        textDirection: TextDirection.ltr,
      );
      tp.layout(maxWidth: size.width - 20);
      tp.paint(canvas, Offset((size.width - tp.width) / 2, (size.height - tp.height) / 2));
      return;
    }

    // Draw line chart
    final maxVal = data.fold<double>(0.0, (a, b) => a > b ? a : b);
    final minVal = data.fold<double>(0.0, (a, b) => a < b ? a : b);
    final range = maxVal - minVal;
    final stepX = size.width / (data.length - 1).clamp(1, 60);

    final path = Path();
    for (int i = 0; i < data.length; i++) {
      final x = i * stepX;
      final normalized = range > 0 ? (data[i] - minVal) / range : 0.5;
      final y = size.height - normalized * (size.height - 20) - 10;
      if (i == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
    }

    canvas.drawPath(
      path,
      Paint()
        ..color = lineColor
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2
        ..strokeCap = StrokeCap.round
        ..strokeJoin = StrokeJoin.round,
    );
  }

  @override
  bool shouldRepaint(covariant _MemoryChartPainter oldDelegate) {
    return oldDelegate.data != data;
  }
}

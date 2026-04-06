import 'dart:async';
import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';

/// Performance statistics window (secondary window, 500x300).
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
  final List<Map<String, dynamic>> _trackStats = [];
  Timer? _pollTimer;

  @override
  void initState() {
    super.initState();
    _pollTimer = Timer.periodic(
      const Duration(milliseconds: 500),
      (_) => _pollStats(),
    );
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    super.dispose();
  }

  Future<void> _pollStats() async {
    // TODO: query main window via WindowMethodChannel when connected
    if (!mounted) return;
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
            // Header
            Row(
              children: [
                Text(l.trackStatistics,
                    style: theme.textTheme.titleMedium),
              ],
            ),
            const SizedBox(height: 8),
            // Stats table
            Expanded(
              child: _trackStats.isEmpty
                  ? Center(
                      child: Text(l.waitingDiagnostics,
                          style: theme.textTheme.bodyMedium?.copyWith(
                                color: theme.colorScheme.onSurfaceVariant,
                              )))
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
                      rows: _trackStats.map((stats) {
                        return DataRow(cells: [
                          DataCell(Text('${stats['slot'] ?? '-'}')),
                          DataCell(Text(
                              '${(stats['fps'] ?? 0.0).toStringAsFixed(1)}')),
                          DataCell(Text(
                              '${(stats['target'] ?? 0.0).toStringAsFixed(1)}')),
                          DataCell(Text(
                              '${(stats['avgDecodeMs'] ?? 0.0).toStringAsFixed(1)}ms')),
                          DataCell(Text(
                              '${(stats['maxDecodeMs'] ?? 0.0).toStringAsFixed(1)}ms')),
                          DataCell(Text(
                            stats['isBottleneck'] == true
                                ? l.bottleneck
                                : l.ok,
                            style: TextStyle(
                              color: stats['isBottleneck'] == true
                                  ? Colors.orange
                                  : Colors.green,
                            ),
                          )),
                        ]);
                      }).toList()),
            ),
            const SizedBox(height: 8),
            // Export button
            Row(
              children: [
                const Spacer(),
                OutlinedButton.icon(
                  onPressed: () {
                    // TODO: implement CSV export
                  },
                  icon: const Icon(Icons.download, size: 16),
                  label: Text(AppLocalizations.of(context)!.export),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

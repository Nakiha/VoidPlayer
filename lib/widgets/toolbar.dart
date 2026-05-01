import 'package:flutter/material.dart';
import '../analysis/analysis_manager.dart';
import '../l10n/app_localizations.dart';
import 'segmented_widget.dart';

/// Top toolbar matching PySide6 ToolBar (40px height, margins: 4).
class AppToolBar extends StatelessWidget {
  final int viewMode; // 0=sideBySide, 1=splitScreen
  final ValueChanged<int> onViewModeChanged;
  final VoidCallback onAddMedia;
  final Future<void> Function() onAnalysis;
  final VoidCallback onProfiler;
  final VoidCallback onSettings;
  final bool viewModeEnabled;
  final bool analysisEnabled;

  const AppToolBar({
    super.key,
    required this.viewMode,
    required this.onViewModeChanged,
    required this.onAddMedia,
    required this.onAnalysis,
    required this.onProfiler,
    required this.onSettings,
    this.viewModeEnabled = false,
    this.analysisEnabled = false,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      height: 40,
      padding: const EdgeInsets.all(4),
      child: Row(
        children: [
          // View mode selector (240x32)
          Opacity(
            opacity: viewModeEnabled ? 1.0 : 0.5,
            child: IgnorePointer(
              ignoring: !viewModeEnabled,
              child: ViewModeSelector(
                currentMode: viewMode,
                onChanged: onViewModeChanged,
              ),
            ),
          ),
          const Spacer(),
          // Add Media button
          SizedBox(
            height: 32,
            child: FilledButton.icon(
              onPressed: onAddMedia,
              icon: const Icon(Icons.add, size: 16),
              label: Text(AppLocalizations.of(context)!.addMedia),
              style: FilledButton.styleFrom(
                padding: const EdgeInsets.symmetric(horizontal: 12),
                visualDensity: VisualDensity.compact,
              ),
            ),
          ),
          const SizedBox(width: 4),
          // Analysis button
          _AnalysisButton(enabled: analysisEnabled, onPressed: onAnalysis),
          const SizedBox(width: 4),
          // Profiler button
          SizedBox(
            width: 32,
            height: 32,
            child: IconButton(
              onPressed: onProfiler,
              icon: const Icon(Icons.speed, size: 18),
              tooltip: AppLocalizations.of(context)!.performanceMonitor,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints.tightFor(width: 32, height: 32),
            ),
          ),
          const SizedBox(width: 4),
          // Settings button
          SizedBox(
            width: 32,
            height: 32,
            child: IconButton(
              onPressed: onSettings,
              icon: const Icon(Icons.settings, size: 18),
              tooltip: AppLocalizations.of(context)!.settings,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints.tightFor(width: 32, height: 32),
            ),
          ),
        ],
      ),
    );
  }
}

class _AnalysisButton extends StatelessWidget {
  final bool enabled;
  final Future<void> Function() onPressed;

  const _AnalysisButton({required this.enabled, required this.onPressed});

  @override
  Widget build(BuildContext context) {
    final mgr = AnalysisManager.instance;
    return ListenableBuilder(
      listenable: mgr,
      builder: (context, _) {
        final theme = Theme.of(context);
        final isWorking =
            mgr.state == AnalysisState.computingHash ||
            mgr.state == AnalysisState.generating ||
            mgr.state == AnalysisState.loading;
        final isError = mgr.state == AnalysisState.error;

        return SizedBox(
          width: 32,
          height: 32,
          child: IconButton(
            onPressed: !enabled || isWorking
                ? null
                : () async {
                    await onPressed();
                    if (!context.mounted) return;
                    final error = mgr.error;
                    if (error?.key == AnalysisErrorKey.cacheLimitExceeded) {
                      ScaffoldMessenger.of(context).showSnackBar(
                        SnackBar(
                          content: Text(
                            _tooltipText(context, mgr, false, true),
                          ),
                        ),
                      );
                    }
                  },
            icon: isWorking
                ? const SizedBox(
                    width: 18,
                    height: 18,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : Icon(
                    isError ? Icons.error_outline : Icons.analytics_outlined,
                    size: 18,
                    color: isError ? theme.colorScheme.error : null,
                  ),
            tooltip: _tooltipText(context, mgr, isWorking, isError),
            padding: EdgeInsets.zero,
            constraints: const BoxConstraints.tightFor(width: 32, height: 32),
          ),
        );
      },
    );
  }

  String _tooltipText(
    BuildContext context,
    AnalysisManager mgr,
    bool isWorking,
    bool isError,
  ) {
    final l = AppLocalizations.of(context)!;
    if (isWorking) {
      return l.analysisGeneratingFor(mgr.generatingFileName ?? '...');
    }
    if (isError) {
      final e = mgr.error;
      if (e == null) return l.analysisErrorUnknown;
      return switch (e.key) {
        AnalysisErrorKey.hashFailed => l.analysisErrorHashFailed(
          e.args.firstOrNull ?? '',
        ),
        AnalysisErrorKey.unsupported => l.analysisErrorUnsupported(
          e.args.firstOrNull ?? '',
        ),
        AnalysisErrorKey.loadFailed => l.analysisErrorLoadFailed(
          e.args.firstOrNull ?? '',
        ),
        AnalysisErrorKey.cacheLimitExceeded =>
          l.analysisErrorCacheLimitExceeded(
            e.args.isNotEmpty ? e.args[0] : '',
            e.args.length > 1 ? e.args[1] : '',
          ),
      };
    }
    return l.analysisClickToAnalyze;
  }
}

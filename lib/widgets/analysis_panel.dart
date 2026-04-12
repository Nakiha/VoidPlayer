import 'package:flutter/material.dart';
import '../analysis/analysis_manager.dart';

/// Floating analysis button on the viewport.
///
/// Three visual states:
/// - **idle**: 36x36 circle icon button (analytics icon) — click triggers analysis
/// - **generating**: 36x36 circular progress indicator, not clickable; hover shows tooltip
/// - **error**: 36x36 red exclamation icon — hover shows error message; click to retry
class AnalysisPanel extends StatefulWidget {
  /// Called when the user triggers analysis for all current tracks.
  /// The callback should iterate tracks, call AnalysisManager.ensureAndLoad for each,
  /// and open analysis windows for successful ones.
  final Future<void> Function() onTriggerAnalysis;

  const AnalysisPanel({
    super.key,
    required this.onTriggerAnalysis,
  });

  @override
  State<AnalysisPanel> createState() => _AnalysisPanelState();
}

class _AnalysisPanelState extends State<AnalysisPanel> {
  bool _hovering = false;

  @override
  void initState() {
    super.initState();
    AnalysisManager.instance.addListener(_onStateChanged);
  }

  @override
  void dispose() {
    AnalysisManager.instance.removeListener(_onStateChanged);
    super.dispose();
  }

  void _onStateChanged() {
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    final mgr = AnalysisManager.instance;
    final isWorking = mgr.state == AnalysisState.computingHash ||
        mgr.state == AnalysisState.generating ||
        mgr.state == AnalysisState.loading;
    final isError = mgr.state == AnalysisState.error;

    return Positioned(
      right: 8,
      top: 8,
      child: MouseRegion(
        onEnter: (_) => setState(() => _hovering = true),
        onExit: (_) => setState(() => _hovering = false),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.end,
          children: [
            // Tooltip on hover
            if (_hovering) _buildTooltip(context, mgr, isWorking, isError),
            const SizedBox(height: 4),
            // The button itself
            _buildButton(context, isWorking, isError),
          ],
        ),
      ),
    );
  }

  Widget _buildButton(BuildContext context, bool isWorking, bool isError) {
    final theme = Theme.of(context);
    const size = 36.0;

    if (isWorking) {
      return SizedBox(
        width: size,
        height: size,
        child: CircularProgressIndicator(
          strokeWidth: 2.5,
          color: theme.colorScheme.primary,
        ),
      );
    }

    return SizedBox(
      width: size,
      height: size,
      child: Material(
        elevation: 2,
        borderRadius: BorderRadius.circular(size / 2),
        color: isError
            ? theme.colorScheme.errorContainer
            : theme.colorScheme.surfaceContainerHigh.withValues(alpha: 0.9),
        child: InkWell(
          borderRadius: BorderRadius.circular(size / 2),
          onTap: isWorking ? null : _onTap,
          child: Icon(
            isError ? Icons.error_outline : Icons.analytics_outlined,
            size: 18,
            color: isError
                ? theme.colorScheme.onErrorContainer
                : theme.colorScheme.onSurfaceVariant,
          ),
        ),
      ),
    );
  }

  Widget _buildTooltip(
      BuildContext context, AnalysisManager mgr, bool isWorking, bool isError) {
    final theme = Theme.of(context);
    String text;
    if (isWorking) {
      final name = mgr.generatingFileName ?? '...';
      text = 'Generating report for $name...';
    } else if (isError) {
      text = mgr.errorMessage ?? 'Unknown error';
    } else {
      text = 'Click to analyze';
    }

    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: theme.colorScheme.inverseSurface.withValues(alpha: 0.9),
        borderRadius: BorderRadius.circular(6),
      ),
      constraints: const BoxConstraints(maxWidth: 240),
      child: Text(
        text,
        style: theme.textTheme.bodySmall?.copyWith(
          color: theme.colorScheme.onInverseSurface,
        ),
        overflow: TextOverflow.ellipsis,
        maxLines: 2,
      ),
    );
  }

  void _onTap() {
    widget.onTriggerAnalysis();
  }
}

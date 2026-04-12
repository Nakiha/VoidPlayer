import 'package:flutter/material.dart';
import '../analysis/analysis_manager.dart';
import '../l10n/app_localizations.dart';

/// Figma-style floating toolbar, toggled with Ctrl+`.
///
/// Appears as a rounded rectangle with a hide button on the left and
/// tool buttons on the right. Currently has only the analysis button.
class AnalysisToolbar extends StatefulWidget {
  /// Whether the toolbar is currently visible.
  final bool visible;

  /// Called when the user wants to hide the toolbar.
  final VoidCallback onHide;

  /// Called when the user triggers analysis for all current tracks.
  final Future<void> Function() onTriggerAnalysis;

  const AnalysisToolbar({
    super.key,
    required this.visible,
    required this.onHide,
    required this.onTriggerAnalysis,
  });

  @override
  State<AnalysisToolbar> createState() => _AnalysisToolbarState();
}

class _AnalysisToolbarState extends State<AnalysisToolbar>
    with SingleTickerProviderStateMixin {
  late final AnimationController _controller;
  late final Animation<double> _opacity;
  late final Animation<Offset> _offset;

  @override
  void initState() {
    super.initState();
    _controller = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 200),
    );
    _opacity = CurvedAnimation(parent: _controller, curve: Curves.easeOut);
    _offset = Tween<Offset>(
      begin: const Offset(0, -0.3),
      end: Offset.zero,
    ).animate(CurvedAnimation(parent: _controller, curve: Curves.easeOut));

    AnalysisManager.instance.addListener(_onStateChanged);
    if (widget.visible) _controller.value = 1.0;
  }

  @override
  void didUpdateWidget(covariant AnalysisToolbar old) {
    super.didUpdateWidget(old);
    if (widget.visible != old.visible) {
      if (widget.visible) {
        _controller.forward();
      } else {
        _controller.reverse();
      }
    }
  }

  @override
  void dispose() {
    AnalysisManager.instance.removeListener(_onStateChanged);
    _controller.dispose();
    super.dispose();
  }

  void _onStateChanged() {
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    if (!_controller.isAnimating && !widget.visible && _controller.isDismissed) {
      return const SizedBox.shrink();
    }

    final theme = Theme.of(context);
    final mgr = AnalysisManager.instance;
    final isWorking = mgr.state == AnalysisState.computingHash ||
        mgr.state == AnalysisState.generating ||
        mgr.state == AnalysisState.loading;
    final isError = mgr.state == AnalysisState.error;

    return FadeTransition(
      opacity: _opacity,
      child: SlideTransition(
        position: _offset,
        child: Positioned(
          right: 8,
          top: 8,
          child: Material(
            elevation: 4,
            borderRadius: BorderRadius.circular(8),
            color: theme.colorScheme.surfaceContainerHigh.withValues(alpha: 0.92),
            child: Tooltip(
              message: _tooltipText(context, mgr, isWorking, isError),
              preferBelow: false,
              waitDuration: const Duration(milliseconds: 400),
              child: Container(
                height: 36,
                padding: const EdgeInsets.symmetric(horizontal: 4),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    // Hide button
                    SizedBox(
                      width: 28,
                      height: 28,
                      child: IconButton(
                        onPressed: widget.onHide,
                        icon: Icon(
                          Icons.chevron_left,
                          size: 18,
                          color: theme.colorScheme.onSurfaceVariant,
                        ),
                        padding: EdgeInsets.zero,
                        constraints: const BoxConstraints.tightFor(width: 28, height: 28),
                        tooltip: null,
                      ),
                    ),
                    const SizedBox(width: 2),
                    // Separator
                    Container(width: 1, height: 20, color: theme.colorScheme.outlineVariant),
                    const SizedBox(width: 4),
                    // Analysis button
                    _AnalysisButton(
                      isWorking: isWorking,
                      isError: isError,
                      onTap: _onTap,
                    ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }

  String _tooltipText(BuildContext context, AnalysisManager mgr, bool isWorking, bool isError) {
    final l = AppLocalizations.of(context)!;
    if (isWorking) {
      final name = mgr.generatingFileName ?? '...';
      return l.analysisGeneratingFor(name);
    } else if (isError) {
      return mgr.errorMessage ?? l.analysisErrorUnknown;
    } else {
      return l.analysisClickToAnalyze;
    }
  }

  void _onTap() {
    widget.onTriggerAnalysis();
  }
}

/// The analysis tool button inside the toolbar.
/// Shows a progress indicator when generating, error icon on error.
class _AnalysisButton extends StatelessWidget {
  final bool isWorking;
  final bool isError;
  final VoidCallback onTap;

  const _AnalysisButton({
    required this.isWorking,
    required this.isError,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    if (isWorking) {
      return const SizedBox(
        width: 28,
        height: 28,
        child: Padding(
          padding: EdgeInsets.all(4),
          child: CircularProgressIndicator(strokeWidth: 2),
        ),
      );
    }

    return SizedBox(
      width: 28,
      height: 28,
      child: InkWell(
        borderRadius: BorderRadius.circular(4),
        onTap: onTap,
        child: Icon(
          isError ? Icons.error_outline : Icons.analytics_outlined,
          size: 18,
          color: isError
              ? theme.colorScheme.error
              : theme.colorScheme.onSurfaceVariant,
        ),
      ),
    );
  }
}

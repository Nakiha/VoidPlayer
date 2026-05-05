import 'package:flutter/material.dart';

import '../feedback/app_feedback.dart';

class AppFeedbackHost extends StatelessWidget {
  const AppFeedbackHost({super.key});

  @override
  Widget build(BuildContext context) {
    final controller = AppFeedbackScope.of(context);
    return AnimatedBuilder(
      animation: controller,
      builder: (context, _) {
        final message = controller.current;
        return IgnorePointer(
          ignoring: message == null,
          child: Align(
            alignment: Alignment.bottomCenter,
            child: AnimatedSwitcher(
              duration: const Duration(milliseconds: 160),
              switchInCurve: Curves.easeOutCubic,
              switchOutCurve: Curves.easeInCubic,
              transitionBuilder: (child, animation) {
                final offset = Tween<Offset>(
                  begin: const Offset(0, 0.25),
                  end: Offset.zero,
                ).animate(animation);
                return FadeTransition(
                  opacity: animation,
                  child: SlideTransition(position: offset, child: child),
                );
              },
              child: message == null
                  ? const SizedBox.shrink(key: ValueKey('empty-feedback'))
                  : _FeedbackBar(
                      key: ValueKey('${message.severity}:${message.text}'),
                      message: message,
                      onDismiss: controller.dismiss,
                    ),
            ),
          ),
        );
      },
    );
  }
}

class _FeedbackBar extends StatelessWidget {
  final AppFeedbackMessage message;
  final VoidCallback onDismiss;

  const _FeedbackBar({
    super.key,
    required this.message,
    required this.onDismiss,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    final accent = switch (message.severity) {
      AppFeedbackSeverity.info => colorScheme.primary,
      AppFeedbackSeverity.success => Colors.green,
      AppFeedbackSeverity.warning => Colors.orange,
      AppFeedbackSeverity.error => colorScheme.error,
    };
    final icon = switch (message.severity) {
      AppFeedbackSeverity.info => Icons.info_outline,
      AppFeedbackSeverity.success => Icons.check_circle_outline,
      AppFeedbackSeverity.warning => Icons.warning_amber_rounded,
      AppFeedbackSeverity.error => Icons.error_outline,
    };
    return SafeArea(
      minimum: const EdgeInsets.fromLTRB(16, 0, 16, 16),
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 560),
        child: Material(
          elevation: 10,
          color: colorScheme.surfaceContainerHighest,
          borderRadius: BorderRadius.circular(8),
          clipBehavior: Clip.antiAlias,
          child: IntrinsicHeight(
            child: Row(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                SizedBox(
                  width: 4,
                  child: DecoratedBox(decoration: BoxDecoration(color: accent)),
                ),
                Flexible(
                  child: Padding(
                    padding: const EdgeInsets.fromLTRB(12, 10, 8, 10),
                    child: Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(icon, size: 18, color: accent),
                        const SizedBox(width: 10),
                        Flexible(
                          child: Text(
                            message.text,
                            maxLines: 3,
                            overflow: TextOverflow.ellipsis,
                            style: theme.textTheme.bodyMedium?.copyWith(
                              color: colorScheme.onSurface,
                            ),
                          ),
                        ),
                        if (message.actionLabel != null &&
                            message.onAction != null) ...[
                          const SizedBox(width: 8),
                          TextButton(
                            onPressed: () {
                              onDismiss();
                              message.onAction?.call();
                            },
                            child: Text(message.actionLabel!),
                          ),
                        ],
                        const SizedBox(width: 4),
                        IconButton(
                          onPressed: onDismiss,
                          icon: const Icon(Icons.close, size: 18),
                          tooltip: MaterialLocalizations.of(
                            context,
                          ).closeButtonTooltip,
                          visualDensity: VisualDensity.compact,
                        ),
                      ],
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

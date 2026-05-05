import 'package:flutter/material.dart';

import '../../l10n/app_localizations.dart';
import '../settings_window.dart';
import '../stats_window.dart';
import 'main_window_media_sections.dart';
import 'main_window_view_model.dart';

class FullScreenPointerCapture extends StatelessWidget {
  final VoidCallback onActivity;

  const FullScreenPointerCapture({super.key, required this.onActivity});

  @override
  Widget build(BuildContext context) {
    return Positioned.fill(
      key: const ValueKey('fullScreenPointerCapture'),
      child: MouseRegion(opaque: false, onHover: (_) => onActivity()),
    );
  }
}

class FullScreenControlsOverlay extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const FullScreenControlsOverlay({
    super.key,
    required this.model,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    return Positioned(
      key: const ValueKey('fullScreenControlsOverlay'),
      left: 12,
      right: 12,
      bottom: 12,
      child: AnimatedOverlaySlot(
        visible: model.fullScreenControlsVisible,
        builder: (context) =>
            FullScreenControlsPanel(model: model, actions: actions),
        transitionBuilder: (context, animation, child) {
          final offset = Tween<Offset>(
            begin: const Offset(0, 0.18),
            end: Offset.zero,
          ).animate(animation);
          return FadeTransition(
            opacity: animation,
            child: SlideTransition(position: offset, child: child),
          );
        },
      ),
    );
  }
}

class DragDropLayer extends StatelessWidget {
  const DragDropLayer({super.key});

  @override
  Widget build(BuildContext context) {
    return Positioned.fill(
      child: IgnorePointer(
        child: Container(
          decoration: BoxDecoration(
            border: Border.all(
              color: Theme.of(
                context,
              ).colorScheme.primary.withValues(alpha: 0.5),
              width: 3,
            ),
            color: Theme.of(
              context,
            ).colorScheme.primary.withValues(alpha: 0.08),
          ),
        ),
      ),
    );
  }
}

class ProfilerOverlaySlot extends StatelessWidget {
  final bool visible;
  final VoidCallback onClose;

  const ProfilerOverlaySlot({
    super.key,
    required this.visible,
    required this.onClose,
  });

  @override
  Widget build(BuildContext context) {
    return Positioned(
      key: const ValueKey('profilerOverlay'),
      top: 48,
      right: 12,
      left: 12,
      child: AnimatedOverlaySlot(
        visible: visible,
        builder: (context) => Align(
          alignment: Alignment.topRight,
          heightFactor: 1,
          child: ConstrainedBox(
            constraints: const BoxConstraints(
              minWidth: 360,
              maxWidth: 560,
              maxHeight: 320,
            ),
            child: _ProfilerOverlay(onClose: onClose),
          ),
        ),
        transitionBuilder: (context, animation, child) {
          final offset = Tween<Offset>(
            begin: const Offset(0.04, 0),
            end: Offset.zero,
          ).animate(animation);
          return FadeTransition(
            opacity: animation,
            child: SlideTransition(position: offset, child: child),
          );
        },
      ),
    );
  }
}

class SettingsOverlaySlot extends StatelessWidget {
  final bool visible;
  final VoidCallback onClose;

  const SettingsOverlaySlot({
    super.key,
    required this.visible,
    required this.onClose,
  });

  @override
  Widget build(BuildContext context) {
    return Positioned.fill(
      key: const ValueKey('settingsOverlay'),
      child: AnimatedOverlaySlot(
        visible: visible,
        builder: (context) => _SettingsDialog(onClose: onClose),
        transitionBuilder: (context, animation, child) {
          return _ModalScrim(
            animation: animation,
            onDismiss: onClose,
            child: child,
          );
        },
      ),
    );
  }
}

typedef OverlayTransitionBuilder =
    Widget Function(
      BuildContext context,
      Animation<double> animation,
      Widget child,
    );

class AnimatedOverlaySlot extends StatefulWidget {
  final bool visible;
  final WidgetBuilder builder;
  final OverlayTransitionBuilder transitionBuilder;

  const AnimatedOverlaySlot({
    super.key,
    required this.visible,
    required this.builder,
    required this.transitionBuilder,
  });

  @override
  State<AnimatedOverlaySlot> createState() => _AnimatedOverlaySlotState();
}

class _AnimatedOverlaySlotState extends State<AnimatedOverlaySlot>
    with SingleTickerProviderStateMixin {
  late final AnimationController _controller;
  late final Animation<double> _animation;
  bool _shouldBuild = false;

  @override
  void initState() {
    super.initState();
    _shouldBuild = widget.visible;
    _controller = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 180),
      reverseDuration: const Duration(milliseconds: 140),
    );
    _animation = CurvedAnimation(
      parent: _controller,
      curve: Curves.easeOutCubic,
      reverseCurve: Curves.easeInCubic,
    );
    if (widget.visible) {
      _controller.value = 1;
    }
  }

  @override
  void didUpdateWidget(covariant AnimatedOverlaySlot oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.visible == oldWidget.visible) return;
    if (widget.visible) {
      setState(() => _shouldBuild = true);
      _controller.forward();
    } else {
      _controller.reverse().whenComplete(() {
        if (!mounted || widget.visible) return;
        setState(() => _shouldBuild = false);
      });
    }
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (!_shouldBuild) return const SizedBox.shrink();
    return widget.transitionBuilder(
      context,
      _animation,
      widget.builder(context),
    );
  }
}

class _ModalScrim extends StatelessWidget {
  final Animation<double> animation;
  final VoidCallback onDismiss;
  final Widget child;

  const _ModalScrim({
    required this.animation,
    required this.onDismiss,
    required this.child,
  });

  @override
  Widget build(BuildContext context) {
    final scale = Tween<double>(begin: 0.96, end: 1).animate(animation);
    return FadeTransition(
      opacity: animation,
      child: Material(
        color: Colors.black.withValues(alpha: 0.32),
        child: Stack(
          children: [
            Positioned.fill(
              child: GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTap: onDismiss,
              ),
            ),
            Center(
              child: ScaleTransition(scale: scale, child: child),
            ),
          ],
        ),
      ),
    );
  }
}

class _ProfilerOverlay extends StatelessWidget {
  final VoidCallback onClose;

  const _ProfilerOverlay({required this.onClose});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return IntrinsicWidth(
      child: Material(
        elevation: 12,
        color: theme.colorScheme.surface,
        borderRadius: BorderRadius.circular(8),
        clipBehavior: Clip.antiAlias,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            SizedBox(
              height: 40,
              child: Row(
                children: [
                  const SizedBox(width: 12),
                  Icon(Icons.speed, size: 18, color: theme.colorScheme.primary),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      AppLocalizations.of(context)!.performanceMonitor,
                      style: theme.textTheme.titleSmall,
                    ),
                  ),
                  IconButton(
                    onPressed: onClose,
                    icon: const Icon(Icons.close, size: 18),
                    tooltip: MaterialLocalizations.of(
                      context,
                    ).closeButtonTooltip,
                  ),
                ],
              ),
            ),
            const Divider(height: 1),
            const Flexible(child: StatsPage()),
          ],
        ),
      ),
    );
  }
}

class _SettingsDialog extends StatelessWidget {
  final VoidCallback onClose;

  const _SettingsDialog({required this.onClose});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return ConstrainedBox(
      constraints: const BoxConstraints(
        maxWidth: 760,
        maxHeight: 560,
        minWidth: 520,
      ),
      child: Material(
        elevation: 16,
        color: theme.colorScheme.surface,
        borderRadius: BorderRadius.circular(8),
        clipBehavior: Clip.antiAlias,
        child: Column(
          children: [
            SizedBox(
              height: 44,
              child: Row(
                children: [
                  const SizedBox(width: 12),
                  Icon(
                    Icons.settings,
                    size: 18,
                    color: theme.colorScheme.primary,
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      AppLocalizations.of(context)!.settings,
                      style: theme.textTheme.titleSmall,
                    ),
                  ),
                  IconButton(
                    onPressed: onClose,
                    icon: const Icon(Icons.close, size: 18),
                    tooltip: MaterialLocalizations.of(
                      context,
                    ).closeButtonTooltip,
                  ),
                ],
              ),
            ),
            const Divider(height: 1),
            const Expanded(child: SettingsPage()),
          ],
        ),
      ),
    );
  }
}

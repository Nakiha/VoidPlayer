import 'package:desktop_drop/desktop_drop.dart';
import 'package:flutter/material.dart';

import '../../l10n/app_localizations.dart';
import '../../widgets/controls_bar.dart';
import '../../widgets/loop_range_bar.dart';
import '../../widgets/media_header.dart';
import '../../widgets/timeline_area.dart';
import '../../widgets/toolbar.dart';
import '../../widgets/viewport_panel.dart';
import '../settings_window.dart';
import '../stats_window.dart';
import '../win32_pointer_button_state_provider.dart';
import 'main_window_state.dart';
import 'main_window_view_model.dart';

class MainWindowView extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const MainWindowView({super.key, required this.model, required this.actions});

  @override
  Widget build(BuildContext context) {
    return DropTarget(
      onDragEntered: (_) => actions.onDragEntered(),
      onDragExited: (_) => actions.onDragExited(),
      onDragDone: (details) {
        final paths = details.files
            .map((f) => f.path)
            .where((path) => path.isNotEmpty)
            .toList();
        if (paths.isNotEmpty) actions.onFilesDropped(paths);
      },
      child: Scaffold(
        body: Stack(
          children: [
            Column(
              children: [
                if (!model.fullScreen)
                  AppToolBar(
                    viewMode: model.viewMode,
                    onViewModeChanged: actions.onViewModeChanged,
                    onAddMedia: actions.onAddMedia,
                    onAnalysis: actions.onAnalysis,
                    onProfiler: actions.onProfiler,
                    onSettings: actions.onSettings,
                    tracks: model.tracks,
                    viewModeEnabled: model.viewModeEnabled,
                    analysisEnabled: model.analysisEnabled,
                  ),
                Expanded(
                  child: ViewportPanel(
                    key: model.viewportKey,
                    textureId: model.textureId,
                    viewportState: model.viewportState,
                    errorText: model.viewportState.errorText,
                    layout: model.layout,
                    onPan: actions.onPan,
                    onSplit: actions.onSplit,
                    onZoom: actions.onZoom,
                    onPointerButton: actions.onPointerButton,
                    onResize: actions.onResize,
                    pointerButtonStateProvider:
                        const Win32PointerButtonStateProvider(),
                  ),
                ),
                if (!model.fullScreen && model.tracks.isNotEmpty)
                  _MediaHeaderForModel(model: model, actions: actions),
                if (!model.fullScreen && model.tracks.isNotEmpty)
                  _ControlsBarForModel(model: model, actions: actions),
                if (!model.fullScreen && model.tracks.isNotEmpty)
                  LoopRangeBar(
                    key: model.loopRangeBarKey,
                    timelineStartWidth: model.timelineStartWidth,
                    enabled: model.loopRangeEnabled,
                    startUs: model.loopStartUs,
                    endUs: model.loopEndUs,
                    durationUs: model.durationUs,
                    onEnabledChanged: actions.onLoopRangeEnabledChanged,
                    onRangeChanged: actions.onLoopRangeChanged,
                    onRangeChangeEnd: actions.onLoopRangeChangeEnd,
                  ),
                if (!model.fullScreen && model.tracks.isNotEmpty)
                  Expanded(
                    flex: 0,
                    child: ValueListenableBuilder<TimelineHoverState>(
                      valueListenable: model.timelineHoverListenable,
                      builder: (context, hover, _) => TimelineArea(
                        entries: model.tracks,
                        currentPtsUs: model.currentPtsUs,
                        onRemoveTrack: actions.onRemoveTrack,
                        onReorder: actions.onReorder,
                        onOffsetChanged: actions.onOffsetChanged,
                        onToggleTrackAudio: actions.onToggleTrackAudio,
                        audibleTrackFileId: model.audibleTrackFileId,
                        syncOffsets: model.syncOffsets,
                        maxEffectiveDurationUs: model.durationUs,
                        hoverPtsUs: hover.hoverPtsUs,
                        sliderHovering: hover.sliderHovering,
                        controlsWidth: model.controlsWidth,
                        onControlsWidthChanged: actions.onControlsWidthChanged,
                        markerPtsUs: model.markerUs,
                        loopRangeEnabled: model.loopRangeEnabled,
                        loopStartUs: model.loopStartUs,
                        loopEndUs: model.loopEndUs,
                      ),
                    ),
                  ),
              ],
            ),
            if (model.fullScreen)
              Positioned.fill(
                key: const ValueKey('fullScreenPointerCapture'),
                child: MouseRegion(
                  opaque: false,
                  onHover: (_) => actions.onFullScreenPointerActivity(),
                ),
              ),
            if (model.fullScreen && model.tracks.isNotEmpty)
              Positioned(
                key: const ValueKey('fullScreenControlsOverlay'),
                left: 12,
                right: 12,
                bottom: 12,
                child: _AnimatedOverlaySlot(
                  visible: model.fullScreenControlsVisible,
                  builder: (context) =>
                      _FullScreenControls(model: model, actions: actions),
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
              ),
            if (model.dragging)
              Positioned.fill(
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
              ),
            Positioned(
              key: const ValueKey('profilerOverlay'),
              top: 48,
              right: 12,
              left: 12,
              child: _AnimatedOverlaySlot(
                visible: model.profilerVisible,
                builder: (context) => Align(
                  alignment: Alignment.topRight,
                  heightFactor: 1,
                  child: ConstrainedBox(
                    constraints: const BoxConstraints(
                      minWidth: 360,
                      maxWidth: 560,
                      maxHeight: 320,
                    ),
                    child: _ProfilerOverlay(onClose: actions.onCloseProfiler),
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
            ),
            Positioned.fill(
              key: const ValueKey('settingsOverlay'),
              child: _AnimatedOverlaySlot(
                visible: model.settingsVisible,
                builder: (context) =>
                    _SettingsDialog(onClose: actions.onCloseSettings),
                transitionBuilder: (context, animation, child) {
                  return _ModalScrim(
                    animation: animation,
                    onDismiss: actions.onCloseSettings,
                    child: child,
                  );
                },
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _MediaHeaderForModel extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const _MediaHeaderForModel({required this.model, required this.actions});

  @override
  Widget build(BuildContext context) {
    return MediaHeaderBar(
      entries: model.tracks,
      onMediaSwapped: actions.onMediaSwapped,
      onRemoveClicked: (slotIndex) {
        if (slotIndex < model.tracks.length) {
          actions.onRemoveTrack(model.tracks[slotIndex].fileId);
        }
      },
    );
  }
}

class _ControlsBarForModel extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const _ControlsBarForModel({required this.model, required this.actions});

  @override
  Widget build(BuildContext context) {
    return ControlsBar(
      timelineKey: model.timelineSliderKey,
      timelineStartWidth: model.timelineStartWidth,
      zoomRatio: model.layout.zoomRatio,
      onZoomChanged: actions.onZoomChanged,
      isPlaying: model.isPlaying,
      isFullScreen: model.fullScreen,
      onToggleFullScreen: actions.onToggleFullScreen,
      onTogglePlay: actions.onTogglePlay,
      onStepForward: actions.onStepForward,
      onStepBackward: actions.onStepBackward,
      currentPtsUs: model.currentPtsUs,
      durationUs: model.durationUs,
      onSeek: actions.onSeek,
      onHoverChanged: actions.onSliderHover,
      markerUs: model.markerUs,
      seekMinUs: model.seekMinUs,
      seekMaxUs: model.seekMaxUs,
    );
  }
}

class _FullScreenControls extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const _FullScreenControls({required this.model, required this.actions});

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return MouseRegion(
      onEnter: (_) => actions.onFullScreenControlsHoverChanged(true),
      onExit: (_) => actions.onFullScreenControlsHoverChanged(false),
      child: Material(
        color: Colors.transparent,
        child: DecoratedBox(
          decoration: BoxDecoration(
            color: colorScheme.surface.withValues(alpha: 0.86),
            borderRadius: BorderRadius.circular(8),
            border: Border.all(
              color: colorScheme.outlineVariant.withValues(alpha: 0.72),
            ),
          ),
          child: Padding(
            padding: const EdgeInsets.all(4),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                _MediaHeaderForModel(model: model, actions: actions),
                _ControlsBarForModel(model: model, actions: actions),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

typedef _OverlayTransitionBuilder =
    Widget Function(
      BuildContext context,
      Animation<double> animation,
      Widget child,
    );

class _AnimatedOverlaySlot extends StatefulWidget {
  final bool visible;
  final WidgetBuilder builder;
  final _OverlayTransitionBuilder transitionBuilder;

  const _AnimatedOverlaySlot({
    required this.visible,
    required this.builder,
    required this.transitionBuilder,
  });

  @override
  State<_AnimatedOverlaySlot> createState() => _AnimatedOverlaySlotState();
}

class _AnimatedOverlaySlotState extends State<_AnimatedOverlaySlot>
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
  void didUpdateWidget(covariant _AnimatedOverlaySlot oldWidget) {
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

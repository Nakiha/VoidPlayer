import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../feedback/app_feedback.dart';
import '../../l10n/app_localizations.dart';
import '../../platform/path_launcher.dart';
import '../../preferences/playback_preferences.dart';
import '../../track_manager.dart';
import '../settings_window.dart';
import '../stats_window.dart';
import 'main_window_media_sections.dart';
import 'main_window_view_model.dart';

const _sidePanelGap = 10.0;
const _sidePanelShadowPadding = 12.0;
const _mediaInfoPanelMaxHeight = 380.0;
const _mediaInfoTableBottomScrollbarPadding = 6.0;
const _settingsDialogMaxWidth = 760.0;
const _settingsDialogMaxHeight = 720.0;
const _settingsDialogMinWidth = 520.0;
const _settingsDialogMinHeight = 360.0;
const _settingsDialogViewportMargin = 16.0;

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
        visible: model.overlays.fullScreenControlsVisible,
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

class FloatingSidePanelsSlot extends StatelessWidget {
  final bool mediaInfoVisible;
  final bool profilerVisible;
  final List<TrackEntry> tracks;
  final VoidCallback onCloseMediaInfo;
  final VoidCallback onCloseProfiler;

  const FloatingSidePanelsSlot({
    super.key,
    required this.mediaInfoVisible,
    required this.profilerVisible,
    required this.tracks,
    required this.onCloseMediaInfo,
    required this.onCloseProfiler,
  });

  @override
  Widget build(BuildContext context) {
    final visible = mediaInfoVisible || profilerVisible;
    return Positioned.fill(
      key: const ValueKey('floatingSidePanels'),
      child: IgnorePointer(
        ignoring: !visible,
        child: LayoutBuilder(
          builder: (context, constraints) {
            final availableWidth = math.max(0.0, constraints.maxWidth - 24);
            final availableHeight = math.max(0.0, constraints.maxHeight - 60);
            return Stack(
              children: [
                Positioned(
                  top: 48,
                  left: 12,
                  child: ConstrainedBox(
                    constraints: BoxConstraints(
                      maxWidth: availableWidth,
                      maxHeight: availableHeight,
                    ),
                    child: _FloatingSidePanelStack(
                      mediaInfoVisible: mediaInfoVisible,
                      profilerVisible: profilerVisible,
                      availableWidth: availableWidth,
                      availableHeight: availableHeight,
                      tracks: tracks,
                      onCloseMediaInfo: onCloseMediaInfo,
                      onCloseProfiler: onCloseProfiler,
                    ),
                  ),
                ),
              ],
            );
          },
        ),
      ),
    );
  }
}

class _FloatingSidePanelStack extends StatelessWidget {
  final bool mediaInfoVisible;
  final bool profilerVisible;
  final double availableWidth;
  final double availableHeight;
  final List<TrackEntry> tracks;
  final VoidCallback onCloseMediaInfo;
  final VoidCallback onCloseProfiler;

  const _FloatingSidePanelStack({
    required this.mediaInfoVisible,
    required this.profilerVisible,
    required this.availableWidth,
    required this.availableHeight,
    required this.tracks,
    required this.onCloseMediaInfo,
    required this.onCloseProfiler,
  });

  @override
  Widget build(BuildContext context) {
    final mediaPanelWidth = math.max(
      0.0,
      availableWidth - _sidePanelShadowPadding,
    );
    final profilerPanelWidth = math.min(
      560.0,
      math.max(360.0, mediaPanelWidth),
    );
    final stackWidth = mediaInfoVisible ? mediaPanelWidth : profilerPanelWidth;
    return SizedBox(
      width: stackWidth + _sidePanelShadowPadding,
      child: ConstrainedBox(
        constraints: BoxConstraints(maxHeight: availableHeight),
        child: Padding(
          padding: const EdgeInsets.only(
            right: _sidePanelShadowPadding,
            bottom: _sidePanelShadowPadding,
          ),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              _AnimatedFloatingPanelSlot(
                visible: mediaInfoVisible,
                child: SizedBox(
                  width: mediaPanelWidth,
                  child: _FloatingPanelFrame(
                    icon: Icons.info_outline,
                    title: AppLocalizations.of(context)!.mediaInfo,
                    onClose: onCloseMediaInfo,
                    minWidth: 0,
                    maxWidth: mediaPanelWidth,
                    child: MediaInfoPage(tracks: tracks),
                  ),
                ),
              ),
              _AnimatedSidePanelGap(
                visible: mediaInfoVisible && profilerVisible,
              ),
              _AnimatedFloatingPanelSlot(
                visible: profilerVisible,
                child: ConstrainedBox(
                  constraints: const BoxConstraints(
                    minWidth: 360,
                    maxWidth: 560,
                    maxHeight: 320,
                  ),
                  child: _FloatingPanelFrame(
                    icon: Icons.speed,
                    title: AppLocalizations.of(context)!.performanceMonitor,
                    onClose: onCloseProfiler,
                    child: const Flexible(child: StatsPage()),
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class SettingsOverlaySlot extends StatelessWidget {
  final bool visible;
  final VoidCallback onClose;
  final ValueChanged<ViewportPixelSizeMode> onViewportPixelSizeModeChanged;

  const SettingsOverlaySlot({
    super.key,
    required this.visible,
    required this.onClose,
    required this.onViewportPixelSizeModeChanged,
  });

  @override
  Widget build(BuildContext context) {
    return Positioned.fill(
      key: const ValueKey('settingsOverlay'),
      child: AnimatedOverlaySlot(
        visible: visible,
        builder: (context) => _SettingsDialog(
          onClose: onClose,
          onViewportPixelSizeModeChanged: onViewportPixelSizeModeChanged,
        ),
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

class _AnimatedFloatingPanelSlot extends StatelessWidget {
  final bool visible;
  final Widget child;

  const _AnimatedFloatingPanelSlot({
    required this.visible,
    required this.child,
  });

  @override
  Widget build(BuildContext context) {
    return AnimatedSize(
      duration: const Duration(milliseconds: 180),
      reverseDuration: const Duration(milliseconds: 140),
      curve: Curves.easeOutCubic,
      alignment: Alignment.topLeft,
      clipBehavior: Clip.none,
      child: AnimatedSwitcher(
        duration: const Duration(milliseconds: 180),
        reverseDuration: const Duration(milliseconds: 140),
        switchInCurve: Curves.easeOutCubic,
        switchOutCurve: Curves.easeInCubic,
        layoutBuilder: (currentChild, previousChildren) {
          return Stack(
            alignment: Alignment.topLeft,
            children: [...previousChildren, ?currentChild],
          );
        },
        transitionBuilder: (child, animation) {
          final offset = Tween<Offset>(
            begin: const Offset(-0.04, 0),
            end: Offset.zero,
          ).animate(animation);
          return FadeTransition(
            opacity: animation,
            child: SlideTransition(position: offset, child: child),
          );
        },
        child: visible
            ? KeyedSubtree(key: const ValueKey('panel'), child: child)
            : const SizedBox.shrink(key: ValueKey('empty')),
      ),
    );
  }
}

class _AnimatedSidePanelGap extends StatelessWidget {
  final bool visible;

  const _AnimatedSidePanelGap({required this.visible});

  @override
  Widget build(BuildContext context) {
    return AnimatedContainer(
      duration: const Duration(milliseconds: 180),
      curve: Curves.easeOutCubic,
      height: visible ? _sidePanelGap : 0,
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
              child: Padding(
                padding: const EdgeInsets.all(_settingsDialogViewportMargin),
                child: ScaleTransition(scale: scale, child: child),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _FloatingPanelFrame extends StatelessWidget {
  final IconData icon;
  final String title;
  final VoidCallback onClose;
  final Widget child;
  final double minWidth;
  final double maxWidth;

  const _FloatingPanelFrame({
    required this.icon,
    required this.title,
    required this.onClose,
    required this.child,
    this.minWidth = 360,
    this.maxWidth = 560,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return ConstrainedBox(
      constraints: BoxConstraints(minWidth: minWidth, maxWidth: maxWidth),
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
                  Icon(icon, size: 18, color: theme.colorScheme.primary),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(title, style: theme.textTheme.titleSmall),
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
            child,
          ],
        ),
      ),
    );
  }
}

class MediaInfoPage extends StatefulWidget {
  final List<TrackEntry> tracks;
  final PathLauncher pathLauncher;

  const MediaInfoPage({
    super.key,
    required this.tracks,
    this.pathLauncher = const LocalPathLauncher(),
  });

  @override
  State<MediaInfoPage> createState() => _MediaInfoPageState();
}

class _MediaInfoPageState extends State<MediaInfoPage> {
  final ScrollController _verticalController = ScrollController();
  final ScrollController _horizontalController = ScrollController();

  @override
  void dispose() {
    _verticalController.dispose();
    _horizontalController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    return ConstrainedBox(
      constraints: const BoxConstraints(maxHeight: _mediaInfoPanelMaxHeight),
      child: widget.tracks.isEmpty
          ? Container(
              width: double.infinity,
              constraints: const BoxConstraints(minHeight: 72),
              alignment: Alignment.center,
              child: Text(
                l.mediaInfoNoTracks,
                textAlign: TextAlign.center,
                style: theme.textTheme.bodyMedium?.copyWith(
                  color: theme.colorScheme.onSurfaceVariant,
                ),
              ),
            )
          : Scrollbar(
              controller: _verticalController,
              thumbVisibility: true,
              child: LayoutBuilder(
                builder: (context, constraints) {
                  return SingleChildScrollView(
                    controller: _verticalController,
                    child: Scrollbar(
                      controller: _horizontalController,
                      thumbVisibility: true,
                      thickness: 6,
                      scrollbarOrientation: ScrollbarOrientation.bottom,
                      child: SingleChildScrollView(
                        controller: _horizontalController,
                        scrollDirection: Axis.horizontal,
                        child: Padding(
                          padding: const EdgeInsets.only(
                            bottom: _mediaInfoTableBottomScrollbarPadding,
                          ),
                          child: ConstrainedBox(
                            constraints: BoxConstraints(
                              minWidth: constraints.maxWidth,
                            ),
                            child: DataTable(
                              horizontalMargin: 12,
                              columnSpacing: 22,
                              headingRowHeight: 34,
                              dataRowMinHeight: 42,
                              dataRowMaxHeight: 42,
                              headingTextStyle: theme.textTheme.labelSmall,
                              dataTextStyle: theme.textTheme.bodySmall,
                              columns: [
                                DataColumn(label: Text(l.track)),
                                DataColumn(label: Text(l.duration)),
                                DataColumn(label: Text(l.mediaInfoStartTime)),
                                DataColumn(label: Text(l.mediaInfoResolution)),
                                DataColumn(label: Text(l.mediaInfoCodec)),
                                DataColumn(label: Text(l.mediaInfoFormat)),
                                DataColumn(label: Text(l.mediaInfoBitrate)),
                                DataColumn(label: Text(l.mediaInfoDecoder)),
                                DataColumn(label: Text(l.open)),
                              ],
                              rows: widget.tracks.map((track) {
                                final info = track.info;
                                return DataRow(
                                  cells: [
                                    DataCell(
                                      _TableText(track.fileName, maxWidth: 180),
                                    ),
                                    DataCell(
                                      Text(_formatTimeUs(info.durationUs, l)),
                                    ),
                                    DataCell(
                                      Text(
                                        _formatTimeUs(
                                          info.startTimeUs,
                                          l,
                                          zeroOk: true,
                                        ),
                                      ),
                                    ),
                                    DataCell(
                                      Text('${info.width}x${info.height}'),
                                    ),
                                    DataCell(
                                      _TableText(
                                        _nonEmpty(
                                          info.codecLongName,
                                          info.codecName,
                                          l,
                                        ),
                                        maxWidth: 260,
                                      ),
                                    ),
                                    DataCell(
                                      _TableText(
                                        _nonEmpty(info.formatName, '', l),
                                        maxWidth: 180,
                                      ),
                                    ),
                                    DataCell(
                                      Text(_formatBitrate(info.bitRate, l)),
                                    ),
                                    DataCell(
                                      _TableText(
                                        _nonEmpty(info.decoderName, '', l),
                                        maxWidth: 180,
                                      ),
                                    ),
                                    DataCell(
                                      IconButton(
                                        onPressed: () => unawaited(
                                          _locateFile(context, track.path),
                                        ),
                                        icon: const Icon(
                                          Icons.folder_open,
                                          size: 18,
                                        ),
                                        tooltip: l.mediaInfoLocateFile,
                                        padding: EdgeInsets.zero,
                                        constraints:
                                            const BoxConstraints.tightFor(
                                              width: 32,
                                              height: 32,
                                            ),
                                      ),
                                    ),
                                  ],
                                );
                              }).toList(),
                            ),
                          ),
                        ),
                      ),
                    ),
                  );
                },
              ),
            ),
    );
  }

  Future<void> _locateFile(BuildContext context, String path) async {
    try {
      await widget.pathLauncher.locateFile(path);
    } catch (error) {
      if (!context.mounted) return;
      AppFeedbackScope.read(
        context,
      ).showError(AppLocalizations.of(context)!.mediaInfoLocateFailed);
    }
  }

  String _formatTimeUs(int value, AppLocalizations l, {bool zeroOk = false}) {
    if (value < 0 || (value == 0 && !zeroOk)) return l.notAvailable;
    final totalMs = value ~/ 1000;
    final hours = totalMs ~/ 3600000;
    final minutes = (totalMs ~/ 60000) % 60;
    final seconds = (totalMs ~/ 1000) % 60;
    final millis = totalMs % 1000;
    if (hours > 0) {
      return '$hours:${minutes.toString().padLeft(2, '0')}:'
          '${seconds.toString().padLeft(2, '0')}.${millis.toString().padLeft(3, '0')}';
    }
    return '$minutes:${seconds.toString().padLeft(2, '0')}.'
        '${millis.toString().padLeft(3, '0')}';
  }

  String _formatBitrate(int value, AppLocalizations l) {
    if (value <= 0) return l.notAvailable;
    if (value >= 1000000) {
      return '${(value / 1000000).toStringAsFixed(2)} Mbps';
    }
    return '${(value / 1000).toStringAsFixed(0)} kbps';
  }

  String _nonEmpty(String primary, String fallback, AppLocalizations l) {
    if (primary.trim().isNotEmpty) return primary;
    if (fallback.trim().isNotEmpty) return fallback;
    return l.notAvailable;
  }
}

class _TableText extends StatelessWidget {
  final String text;
  final double maxWidth;

  const _TableText(this.text, {required this.maxWidth});

  @override
  Widget build(BuildContext context) {
    return ConstrainedBox(
      constraints: BoxConstraints(maxWidth: maxWidth),
      child: Text(text, maxLines: 1, overflow: TextOverflow.ellipsis),
    );
  }
}

class _SettingsDialog extends StatelessWidget {
  final VoidCallback onClose;
  final ValueChanged<ViewportPixelSizeMode> onViewportPixelSizeModeChanged;

  const _SettingsDialog({
    required this.onClose,
    required this.onViewportPixelSizeModeChanged,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return LayoutBuilder(
      builder: (context, constraints) {
        final availableWidth = constraints.maxWidth.isFinite
            ? constraints.maxWidth
            : _settingsDialogMaxWidth;
        final availableHeight = constraints.maxHeight.isFinite
            ? constraints.maxHeight
            : _settingsDialogMaxHeight;
        final width = math.min(_settingsDialogMaxWidth, availableWidth);
        final height = math.min(_settingsDialogMaxHeight, availableHeight);
        final minWidth = math.min(_settingsDialogMinWidth, width);
        final minHeight = math.min(_settingsDialogMinHeight, height);

        return ConstrainedBox(
          constraints: BoxConstraints(
            minWidth: minWidth,
            maxWidth: width,
            minHeight: minHeight,
            maxHeight: height,
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
                Expanded(
                  child: SettingsPage(
                    onViewportPixelSizeModeChanged:
                        onViewportPixelSizeModeChanged,
                  ),
                ),
              ],
            ),
          ),
        );
      },
    );
  }
}

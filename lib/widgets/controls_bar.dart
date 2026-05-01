import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';
import 'zoom_combo_box.dart';
import 'time_label.dart';
import 'timeline_slider.dart';

enum _ControlsBarItem { zoom, fullscreen, step, play, time }

/// Bottom playback controls bar matching PySide6 ControlsBar (40px height).
class ControlsBar extends StatelessWidget {
  static const double _leftPadding = 4.0;
  static const double _zoomWidth = 76.0;
  static const double _iconWidth = 32.0;
  static const double _gapWidth = 4.0;
  static const double _timeWidth = 128.0;
  static const double _zoomHideWidth =
      _zoomWidth + _gapWidth + _iconWidth * 4 + _gapWidth + _timeWidth;
  static const double _fullscreenHideWidth =
      _iconWidth * 4 + _gapWidth + _timeWidth;
  static const double _stepHideWidth = _iconWidth * 3 + _gapWidth + _timeWidth;
  static const double _playHideWidth = _iconWidth + _gapWidth + _timeWidth;
  static const double _timeHideWidth = _timeWidth;

  final double zoomRatio;
  final ValueChanged<double> onZoomChanged;
  final bool isPlaying;
  final bool isFullScreen;
  final VoidCallback onTogglePlay;
  final VoidCallback onToggleFullScreen;
  final VoidCallback onStepForward;
  final VoidCallback onStepBackward;
  final int currentPtsUs;
  final int durationUs;
  final ValueChanged<int> onSeek;
  final void Function(int hoverUs, bool hovering)? onHoverChanged;
  final double timelineStartWidth;
  final List<int> markerUs;
  final int? seekMinUs;
  final int? seekMaxUs;
  final Key? timelineKey;

  const ControlsBar({
    super.key,
    required this.zoomRatio,
    required this.onZoomChanged,
    required this.isPlaying,
    required this.isFullScreen,
    required this.onTogglePlay,
    required this.onToggleFullScreen,
    required this.onStepForward,
    required this.onStepBackward,
    required this.currentPtsUs,
    required this.durationUs,
    required this.onSeek,
    this.onHoverChanged,
    this.timelineStartWidth = 349,
    this.markerUs = const [],
    this.seekMinUs,
    this.seekMaxUs,
    this.timelineKey,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      height: 40,
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(
        children: [
          SizedBox(
            width: timelineStartWidth,
            child: LayoutBuilder(
              builder: (context, constraints) {
                final visibleItems = _visibleItemsForWidth(
                  constraints.maxWidth,
                );
                final showZoom = visibleItems.contains(_ControlsBarItem.zoom);
                final showFullscreen = visibleItems.contains(
                  _ControlsBarItem.fullscreen,
                );
                final showStep = visibleItems.contains(_ControlsBarItem.step);
                final showPlay = visibleItems.contains(_ControlsBarItem.play);
                final showTime = visibleItems.contains(_ControlsBarItem.time);

                return Padding(
                  padding: const EdgeInsets.symmetric(horizontal: _leftPadding),
                  child: Row(
                    children: [
                      if (showZoom) ...[
                        ZoomComboBox(
                          value: zoomRatio,
                          onChanged: onZoomChanged,
                        ),
                        const SizedBox(width: 4),
                      ],
                      if (showFullscreen)
                        _ControlIconButton(
                          onPressed: onToggleFullScreen,
                          icon: isFullScreen
                              ? Icons.fullscreen_exit
                              : Icons.fullscreen,
                          iconSize: 20,
                          tooltip: isFullScreen
                              ? AppLocalizations.of(context)!.exitFullScreen
                              : AppLocalizations.of(context)!.enterFullScreen,
                        ),
                      if (showStep)
                        _ControlIconButton(
                          onPressed: onStepBackward,
                          icon: Icons.skip_previous,
                          iconSize: 18,
                          tooltip: AppLocalizations.of(context)!.previousFrame,
                        ),
                      if (showPlay)
                        _ControlIconButton(
                          onPressed: onTogglePlay,
                          icon: isPlaying ? Icons.pause : Icons.play_arrow,
                          iconSize: 20,
                          tooltip: isPlaying
                              ? AppLocalizations.of(context)!.pause
                              : AppLocalizations.of(context)!.play,
                        ),
                      if (showStep)
                        _ControlIconButton(
                          onPressed: onStepForward,
                          icon: Icons.skip_next,
                          iconSize: 18,
                          tooltip: AppLocalizations.of(context)!.nextFrame,
                        ),
                      if (showTime) ...[
                        if (showZoom || showFullscreen || showStep || showPlay)
                          const SizedBox(width: 4),
                        SizedBox(
                          width: _timeWidth,
                          child: ClipRect(
                            child: Align(
                              alignment: Alignment.centerLeft,
                              child: TimeLabel(
                                currentUs: currentPtsUs,
                                totalUs: durationUs,
                              ),
                            ),
                          ),
                        ),
                      ] else
                        const Spacer(),
                    ],
                  ),
                );
              },
            ),
          ),
          // Timeline slider (expanded)
          Expanded(
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 8),
              child: TimelineSlider(
                key: timelineKey,
                currentUs: currentPtsUs,
                durationUs: durationUs,
                onSeek: onSeek,
                onHoverChanged: onHoverChanged,
                markerUs: markerUs,
                seekMinUs: seekMinUs,
                seekMaxUs: seekMaxUs,
              ),
            ),
          ),
        ],
      ),
    );
  }

  Set<_ControlsBarItem> _visibleItemsForWidth(double width) {
    if (!width.isFinite) {
      width = timelineStartWidth;
    }
    final contentWidth = width - _leftPadding * 2;
    return {
      if (contentWidth >= _zoomHideWidth) _ControlsBarItem.zoom,
      if (contentWidth >= _fullscreenHideWidth) _ControlsBarItem.fullscreen,
      if (contentWidth >= _stepHideWidth) _ControlsBarItem.step,
      if (contentWidth >= _playHideWidth) _ControlsBarItem.play,
      if (contentWidth >= _timeHideWidth) _ControlsBarItem.time,
    };
  }
}

class _ControlIconButton extends StatelessWidget {
  final VoidCallback onPressed;
  final IconData icon;
  final double iconSize;
  final String tooltip;

  const _ControlIconButton({
    required this.onPressed,
    required this.icon,
    required this.iconSize,
    required this.tooltip,
  });

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 32,
      height: 32,
      child: IconButton(
        onPressed: onPressed,
        icon: Icon(icon, size: iconSize),
        tooltip: tooltip,
        padding: EdgeInsets.zero,
        constraints: const BoxConstraints.tightFor(width: 32, height: 32),
      ),
    );
  }
}

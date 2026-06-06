import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';
import '../platform/pointer_button_state_provider.dart';
import '../utils/pointer_gesture_utils.dart';
import '../video_renderer_controller.dart';
import '../viewport/viewport_display_state.dart';
import '../viewport/viewport_interaction.dart';
import 'axtree_region.dart';

class ViewportPanel extends StatefulWidget {
  final int? textureId;
  final ViewportDisplayState viewportState;
  final String? errorText;
  final LayoutState layout;

  final void Function(Offset delta) onPan;
  final void Function(double normalizedX) onSplit;
  final void Function(double factor, Offset localPosition) onZoom;
  final void Function(bool panning, bool splitting) onPointerButton;
  final void Function(int width, int height, double devicePixelRatio)? onResize;
  final PointerButtonStateProvider pointerButtonStateProvider;
  final bool nativePlaybackAvailable;
  final ViewportInteractionPolicy interactionPolicy;

  const ViewportPanel({
    super.key,
    required this.textureId,
    required this.viewportState,
    this.errorText,
    required this.layout,
    required this.onPan,
    required this.onSplit,
    required this.onZoom,
    required this.onPointerButton,
    this.onResize,
    this.pointerButtonStateProvider = emptyPointerButtonStateProvider,
    this.nativePlaybackAvailable = true,
    this.interactionPolicy = defaultViewportInteractionPolicy,
  });

  @override
  State<ViewportPanel> createState() => _ViewportPanelState();
}

class _ViewportPanelState extends State<ViewportPanel> {
  static const double _splitHandlePhysicalWidth = 4.0;
  static const double _splitHandleTouchWidth = 28.0;
  static const double _splitHandleVisualWidth = 24.0;
  static const double _splitHandleVisualHeight = 38.0;
  static const double _wheelScrollDeltaPerStep = 120.0;
  static const double _wheelZoomFactorPerStep = 1.1;

  bool _panning = false;
  bool _splitting = false;
  bool _splitHandleDragging = false;
  bool _panZoomScaling = false;
  Offset _lastMouseLocalPos = Offset.zero;
  Size _lastReportedLogicalSize = Size.zero;
  double _lastReportedDevicePixelRatio = 0.0;
  double _lastPanZoomScale = 1.0;

  void _syncDragButtons(
    int buttons,
    Offset localPosition, {
    bool allowWin32Recovery = false,
  }) {
    if (_splitHandleDragging) return;

    var dragIntent = widget.interactionPolicy.dragIntentForButtons(buttons);
    var wantsPan = dragIntent == ViewportDragIntent.pan;
    const wantsSplit = false;
    if (!wantsPan && !wantsSplit && allowWin32Recovery && buttons == 0) {
      dragIntent = widget.pointerButtonStateProvider.isPrimaryButtonDown
          ? ViewportDragIntent.pan
          : ViewportDragIntent.none;
      wantsPan = dragIntent == ViewportDragIntent.pan;
    }

    if (!wantsPan && !wantsSplit) {
      if (_panning || _splitting) {
        _panning = false;
        _splitting = false;
        widget.onPointerButton(false, false);
      }
      return;
    }

    if (wantsPan != _panning || wantsSplit != _splitting) {
      _panning = wantsPan;
      _splitting = wantsSplit;
      _lastMouseLocalPos = localPosition;
      widget.onPointerButton(_panning, _splitting);
    }
  }

  void _updateSplitFromLocalX(BuildContext context, double localX) {
    if (!_splitting || widget.layout.mode != LayoutMode.splitScreen) return;
    final box = context.findRenderObject() as RenderBox;
    if (box.size.width <= 0) return;
    widget.onSplit(localX / box.size.width);
  }

  bool _isOnSplitHandle(BuildContext context, Offset localPosition) {
    if (widget.layout.mode != LayoutMode.splitScreen) return false;
    final box = context.findRenderObject() as RenderBox;
    if (box.size.width <= 0) return false;
    final handleX = box.size.width * widget.layout.splitPos;
    return (localPosition.dx - handleX).abs() <= _splitHandleTouchWidth / 2;
  }

  void _startSplitHandleDrag(BuildContext context, double viewportLocalX) {
    _splitHandleDragging = true;
    _splitting = true;
    _panning = false;
    _lastMouseLocalPos = Offset(viewportLocalX, 0);
    widget.onPointerButton(false, true);
    _updateSplitFromLocalX(context, viewportLocalX);
  }

  void _updateSplitHandleDrag(BuildContext context, double viewportLocalX) {
    if (!_splitHandleDragging) return;
    _lastMouseLocalPos = Offset(viewportLocalX, 0);
    _updateSplitFromLocalX(context, viewportLocalX);
  }

  void _endSplitHandleDrag() {
    if (!_splitHandleDragging) return;
    _splitHandleDragging = false;
    _splitting = false;
    widget.onPointerButton(false, false);
  }

  void _cancelPointerDragState() {
    if (!_panning && !_splitting && !_splitHandleDragging) return;
    _panning = false;
    _splitting = false;
    _splitHandleDragging = false;
    widget.onPointerButton(false, false);
  }

  void _resetPanZoom() {
    _lastPanZoomScale = 1.0;
    _panZoomScaling = false;
  }

  void _zoomByFactor(double factor, Offset physicalLocalPosition) {
    if (factor <= 0 || !factor.isFinite || factor == 1.0) return;
    widget.onZoom(factor, physicalLocalPosition);
  }

  void _zoomByWheelDelta(double scrollDelta, Offset physicalLocalPosition) {
    if (scrollDelta == 0.0 || !scrollDelta.isFinite) return;
    final factor = math
        .pow(_wheelZoomFactorPerStep, -scrollDelta / _wheelScrollDeltaPerStep)
        .toDouble();
    _zoomByFactor(factor, physicalLocalPosition);
  }

  void _clampSplitOnExit(BuildContext context, Offset localPosition) {
    if (!_splitting || widget.layout.mode != LayoutMode.splitScreen) return;
    final box = context.findRenderObject() as RenderBox;
    final width = box.size.width;
    if (width <= 0) return;

    if (localPosition.dx <= 0.0) {
      widget.onSplit(0.0);
    } else if (localPosition.dx >= width) {
      widget.onSplit(1.0);
    }
  }

  void _maybeReportResize(
    BuildContext context, {
    required double logicalWidth,
    required double logicalHeight,
  }) {
    final logicalSize = Size(logicalWidth, logicalHeight);
    final devicePixelRatio = View.of(context).devicePixelRatio;
    if ((logicalSize != _lastReportedLogicalSize ||
            devicePixelRatio != _lastReportedDevicePixelRatio) &&
        logicalWidth > 0 &&
        logicalHeight > 0) {
      _cancelPointerDragState();
      _lastReportedLogicalSize = logicalSize;
      _lastReportedDevicePixelRatio = devicePixelRatio;
      final physicalWidth = (logicalWidth * devicePixelRatio).round();
      final physicalHeight = (logicalHeight * devicePixelRatio).round();
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted) return;
        widget.onResize?.call(physicalWidth, physicalHeight, devicePixelRatio);
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        _maybeReportResize(
          context,
          logicalWidth: constraints.maxWidth,
          logicalHeight: constraints.maxHeight,
        );
        return IndexedStack(
          index: widget.viewportState.stackIndex,
          sizing: StackFit.expand,
          children: [
            // State 0: Loading
            Center(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  SizedBox(
                    width: 48,
                    height: 48,
                    child: CircularProgressIndicator(
                      strokeWidth: 3,
                      color: Theme.of(context).colorScheme.primary,
                    ),
                  ),
                  const SizedBox(height: 8),
                  Text(
                    AppLocalizations.of(context)!.initializing,
                    style: TextStyle(
                      color: Theme.of(context).colorScheme.onSurfaceVariant,
                    ),
                  ),
                ],
              ),
            ),
            // State 1: Empty
            Center(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(
                    Icons.videocam_outlined,
                    size: 64,
                    color: Theme.of(context).colorScheme.onSurfaceVariant,
                  ),
                  const SizedBox(height: 8),
                  Text(
                    widget.nativePlaybackAvailable
                        ? AppLocalizations.of(context)!.emptyHint
                        : AppLocalizations.of(
                            context,
                          )!.platformPlaybackUnavailable,
                    style: TextStyle(
                      color: Theme.of(context).colorScheme.onSurfaceVariant,
                    ),
                  ),
                ],
              ),
            ),
            // State 2: Active (Texture + mouse listener)
            _buildActiveViewport(context),
            // State 3: Error
            Center(
              child: Padding(
                padding: const EdgeInsets.all(24),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Icon(
                      Icons.error_outline,
                      size: 56,
                      color: Theme.of(context).colorScheme.error,
                    ),
                    const SizedBox(height: 10),
                    Text(
                      widget.errorText ?? 'Failed to load media',
                      textAlign: TextAlign.center,
                      style: TextStyle(
                        color: Theme.of(context).colorScheme.onSurfaceVariant,
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ],
        );
      },
    );
  }

  Widget _buildActiveViewport(BuildContext context) {
    if (widget.textureId == null) {
      return const SizedBox.shrink();
    }
    final devicePixelRatio = View.of(context).devicePixelRatio;
    return AxTreeRegion(
      label: 'Video viewport',
      value: widget.layout.mode == LayoutMode.splitScreen
          ? 'Split ${axPercent(widget.layout.splitPos)}'
          : null,
      image: true,
      child: MouseRegion(
        onEnter: (e) {
          _lastMouseLocalPos = e.localPosition;
          _syncDragButtons(
            e.buttons,
            e.localPosition,
            allowWin32Recovery: true,
          );
        },
        onExit: (e) => _clampSplitOnExit(context, e.localPosition),
        onHover: (e) {
          if (!_panning && !_splitting) {
            _lastMouseLocalPos = e.localPosition;
          }
          _syncDragButtons(e.buttons, e.localPosition);
          _updateSplitFromLocalX(context, e.localPosition.dx);
        },
        child: Listener(
          onPointerDown: (e) {
            if ((e.buttons & kPrimaryButton) != 0) {
              if (_isOnSplitHandle(context, e.localPosition)) {
                _startSplitHandleDrag(context, e.localPosition.dx);
                return;
              }
              _panning = false;
              _splitting = false;
              _lastMouseLocalPos = e.localPosition;
            }
            _syncDragButtons(e.buttons, e.localPosition);
            _updateSplitFromLocalX(context, e.localPosition.dx);
          },
          onPointerUp: (e) {
            if (_splitHandleDragging) {
              _endSplitHandleDrag();
              return;
            }
            _syncDragButtons(
              e.buttons,
              e.localPosition,
              allowWin32Recovery: true,
            );
          },
          onPointerCancel: (_) {
            if (_splitHandleDragging) {
              _endSplitHandleDrag();
              return;
            }
            _syncDragButtons(0, _lastMouseLocalPos, allowWin32Recovery: true);
          },
          onPointerMove: (e) {
            if (_splitHandleDragging) {
              if ((e.buttons & kPrimaryButton) == 0) {
                _endSplitHandleDrag();
              } else {
                _updateSplitHandleDrag(context, e.localPosition.dx);
              }
              return;
            }
            _syncDragButtons(
              e.buttons,
              e.localPosition,
              allowWin32Recovery: _panning || _splitting,
            );
            if (!_panning && !_splitting) return;
            final logicalDelta = e.localPosition - _lastMouseLocalPos;
            final physicalDelta = logicalDelta * devicePixelRatio;
            _lastMouseLocalPos = e.localPosition;

            if (_panning) {
              widget.onPan(physicalDelta);
            }

            _updateSplitFromLocalX(context, e.localPosition.dx);
          },
          onPointerSignal: (e) {
            if (e is PointerScrollEvent) {
              _zoomByWheelDelta(
                e.scrollDelta.dy,
                e.localPosition * devicePixelRatio,
              );
            }
          },
          onPointerPanZoomStart: (_) => _resetPanZoom(),
          onPointerPanZoomUpdate: (e) {
            if (e.scale > 0 && e.scale.isFinite && _lastPanZoomScale > 0) {
              final previousScale = _lastPanZoomScale;
              final scaleDelta = e.scale / _lastPanZoomScale;
              _lastPanZoomScale = e.scale;
              final scaleIntent =
                  _panZoomScaling ||
                  isPanZoomScaleIntent(
                    scale: e.scale,
                    lastScale: previousScale,
                  );
              if (scaleIntent && scaleDelta != 1.0) {
                _panZoomScaling = true;
                _zoomByFactor(scaleDelta, e.localPosition * devicePixelRatio);
                return;
              }
            }

            if (_panZoomScaling) return;
            final physicalPanDelta = e.panDelta * devicePixelRatio;
            if (physicalPanDelta != Offset.zero) {
              widget.onPan(physicalPanDelta);
            }
          },
          onPointerPanZoomEnd: (_) => _resetPanZoom(),
          child: Stack(
            fit: StackFit.expand,
            children: [
              ExcludeSemantics(child: Texture(textureId: widget.textureId!)),
              if (widget.layout.mode == LayoutMode.splitScreen)
                _buildSplitHandleSemantics(context, devicePixelRatio),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildSplitHandleSemantics(
    BuildContext context,
    double devicePixelRatio,
  ) {
    final splitPos = widget.layout.splitPos;
    return Positioned.fill(
      child: Semantics(
        container: true,
        slider: true,
        label: 'Viewport split handle',
        value: axPercent(splitPos),
        increasedValue: axPercent((splitPos + 0.05).clamp(0.0, 1.0)),
        decreasedValue: axPercent((splitPos - 0.05).clamp(0.0, 1.0)),
        onIncrease: () => widget.onSplit((splitPos + 0.05).clamp(0.0, 1.0)),
        onDecrease: () => widget.onSplit((splitPos - 0.05).clamp(0.0, 1.0)),
        child: ExcludeSemantics(
          child: _buildSplitHandle(context, devicePixelRatio),
        ),
      ),
    );
  }

  Widget _buildSplitHandle(BuildContext context, double devicePixelRatio) {
    final viewportContext = context;
    final logicalWidth = (_splitHandlePhysicalWidth / devicePixelRatio).clamp(
      2.0,
      4.0,
    );
    return LayoutBuilder(
      builder: (context, constraints) {
        final left =
            constraints.maxWidth * widget.layout.splitPos - logicalWidth / 2;
        final touchLeft =
            constraints.maxWidth * widget.layout.splitPos -
            _splitHandleTouchWidth / 2;
        return Stack(
          children: [
            Positioned(
              left: left,
              top: 0,
              bottom: 0,
              width: logicalWidth,
              child: IgnorePointer(
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    color: Theme.of(
                      context,
                    ).colorScheme.outline.withValues(alpha: 0.55),
                  ),
                ),
              ),
            ),
            Positioned(
              left: touchLeft,
              top: 0,
              bottom: 0,
              width: _splitHandleTouchWidth,
              child: MouseRegion(
                cursor: SystemMouseCursors.resizeColumn,
                child: Listener(
                  behavior: HitTestBehavior.translucent,
                  onPointerDown: (event) {
                    if ((event.buttons & kPrimaryButton) == 0) return;
                    _startSplitHandleDrag(
                      viewportContext,
                      touchLeft + event.localPosition.dx,
                    );
                  },
                  onPointerMove: (event) {
                    if ((event.buttons & kPrimaryButton) == 0) {
                      _endSplitHandleDrag();
                      return;
                    }
                    _updateSplitHandleDrag(
                      viewportContext,
                      touchLeft + event.localPosition.dx,
                    );
                  },
                  onPointerUp: (_) => _endSplitHandleDrag(),
                  onPointerCancel: (_) => _endSplitHandleDrag(),
                  child: Center(child: _SplitHandleGrip()),
                ),
              ),
            ),
          ],
        );
      },
    );
  }
}

class _SplitHandleGrip extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Container(
      width: _ViewportPanelState._splitHandleVisualWidth,
      height: _ViewportPanelState._splitHandleVisualHeight,
      decoration: BoxDecoration(
        color: colorScheme.surfaceContainerHighest.withValues(alpha: 0.82),
        borderRadius: BorderRadius.circular(6),
        border: Border.all(
          color: colorScheme.outlineVariant.withValues(alpha: 0.7),
        ),
        boxShadow: [
          BoxShadow(
            color: Colors.black.withValues(alpha: 0.18),
            blurRadius: 8,
            offset: const Offset(0, 2),
          ),
        ],
      ),
      child: RotatedBox(
        quarterTurns: 1,
        child: Icon(
          Icons.drag_handle,
          size: 18,
          color: colorScheme.onSurfaceVariant,
        ),
      ),
    );
  }
}

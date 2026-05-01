import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';
import '../video_renderer_controller.dart';
import '../windows/win32ffi.dart';

/// Three-state viewport matching PySide6 ViewportPanel.
/// States: 0=loading, 1=empty, 2=active(Texture with mouse interaction).
class ViewportPanel extends StatefulWidget {
  final int? textureId;
  final int viewportState; // 0=loading, 1=empty, 2=active
  final String? errorText;
  final LayoutState layout;

  final void Function(Offset delta) onPan;
  final void Function(double normalizedX) onSplit;
  final void Function(double delta, Offset localPosition) onZoom;
  final void Function(bool panning, bool splitting) onPointerButton;
  final void Function(int width, int height, double devicePixelRatio)? onResize;

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
  });

  @override
  State<ViewportPanel> createState() => _ViewportPanelState();
}

class _ViewportPanelState extends State<ViewportPanel> {
  static const double _splitHandlePhysicalWidth = 4.0;

  bool _panning = false;
  bool _splitting = false;
  bool _splitHandleDragging = false;
  Offset _lastMouseLocalPos = Offset.zero;
  Size _lastReportedLogicalSize = Size.zero;
  double _lastReportedDevicePixelRatio = 0.0;

  void _syncDragButtons(int buttons, Offset localPosition) {
    if (_splitHandleDragging) return;

    final wantsPan =
        (buttons & kPrimaryButton) != 0 || Win32FFI.isLeftMouseButtonDown();
    final wantsSplit =
        (buttons & kSecondaryButton) != 0 || Win32FFI.isRightMouseButtonDown();

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
      _lastReportedLogicalSize = logicalSize;
      _lastReportedDevicePixelRatio = devicePixelRatio;
      widget.onResize?.call(
        (logicalWidth * devicePixelRatio).round(),
        (logicalHeight * devicePixelRatio).round(),
        devicePixelRatio,
      );
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
          index: widget.viewportState,
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
                    AppLocalizations.of(context)!.emptyHint,
                    style: TextStyle(
                      color: Theme.of(context).colorScheme.onSurfaceVariant,
                    ),
                  ),
                ],
              ),
            ),
            // State 2: Active (Texture + mouse listener)
            _buildActiveViewport(context),
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
    return MouseRegion(
      onEnter: (e) => _syncDragButtons(e.buttons, e.localPosition),
      onExit: (e) => _clampSplitOnExit(context, e.localPosition),
      onHover: (e) {
        _syncDragButtons(e.buttons, e.localPosition);
        _updateSplitFromLocalX(context, e.localPosition.dx);
      },
      child: Listener(
        onPointerDown: (e) {
          _syncDragButtons(e.buttons, e.localPosition);
          _updateSplitFromLocalX(context, e.localPosition.dx);
        },
        onPointerUp: (e) {
          _syncDragButtons(e.buttons, e.localPosition);
        },
        onPointerCancel: (_) {
          _syncDragButtons(0, _lastMouseLocalPos);
        },
        onPointerMove: (e) {
          _syncDragButtons(e.buttons, e.localPosition);
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
            widget.onZoom(e.scrollDelta.dy, e.localPosition * devicePixelRatio);
          }
        },
        onPointerPanZoomUpdate: (e) {
          // Trackpad two-finger scroll -> zoom
          final panDelta = e.pan.dy;
          if (panDelta != 0.0) {
            widget.onZoom(panDelta, e.localPosition * devicePixelRatio);
          }
        },
        child: Stack(
          fit: StackFit.expand,
          children: [
            Texture(textureId: widget.textureId!),
            if (widget.layout.mode == LayoutMode.splitScreen)
              _buildSplitHandle(context, devicePixelRatio),
          ],
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
    return Positioned.fill(
      child: LayoutBuilder(
        builder: (context, constraints) {
          final left =
              constraints.maxWidth * widget.layout.splitPos - logicalWidth / 2;
          return Stack(
            children: [
              Positioned(
                left: left,
                top: 0,
                bottom: 0,
                width: logicalWidth,
                child: MouseRegion(
                  cursor: SystemMouseCursors.resizeColumn,
                  child: Listener(
                    behavior: HitTestBehavior.opaque,
                    onPointerDown: (event) {
                      if ((event.buttons & kPrimaryButton) == 0) return;
                      _startSplitHandleDrag(
                        viewportContext,
                        left + event.localPosition.dx,
                      );
                    },
                    onPointerMove: (event) {
                      if ((event.buttons & kPrimaryButton) == 0) {
                        _endSplitHandleDrag();
                        return;
                      }
                      _updateSplitHandleDrag(
                        viewportContext,
                        left + event.localPosition.dx,
                      );
                    },
                    onPointerUp: (_) => _endSplitHandleDrag(),
                    onPointerCancel: (_) => _endSplitHandleDrag(),
                  ),
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}

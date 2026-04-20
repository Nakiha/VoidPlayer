import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';
import '../video_renderer_controller.dart';

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
  final void Function(int width, int height)? onResize;

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
  bool _panning = false;
  bool _splitting = false;
  Offset _lastMousePos = Offset.zero;
  Size _lastReportedSize = Size.zero;

  @override
  Widget build(BuildContext context) {
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
              Text(AppLocalizations.of(context)!.initializing,
                  style: TextStyle(
                    color: Theme.of(context).colorScheme.onSurfaceVariant,
                  )),
            ],
          ),
        ),
        // State 1: Empty
        Center(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(Icons.videocam_outlined,
                  size: 64,
                  color: Theme.of(context).colorScheme.onSurfaceVariant),
              const SizedBox(height: 8),
              Text(AppLocalizations.of(context)!.emptyHint,
                  style: TextStyle(
                    color: Theme.of(context).colorScheme.onSurfaceVariant,
                  )),
            ],
          ),
        ),
        // State 2: Active (Texture + mouse listener)
        _buildActiveViewport(context),
      ],
    );
  }

  Widget _buildActiveViewport(BuildContext context) {
    if (widget.textureId == null) {
      return const SizedBox.shrink();
    }
    return LayoutBuilder(
      builder: (context, constraints) {
        final w = constraints.maxWidth;
        final h = constraints.maxHeight;
        final size = Size(w, h);
        if (size != _lastReportedSize && w > 0 && h > 0) {
          _lastReportedSize = size;
          widget.onResize?.call(w.toInt(), h.toInt());
        }
        return Listener(
          onPointerDown: (e) {
            if ((e.buttons & kPrimaryButton) != 0) {
              _panning = true;
              _lastMousePos = e.position;
              widget.onPointerButton(true, false);
            } else if ((e.buttons & kSecondaryButton) != 0) {
              _splitting = true;
              _lastMousePos = e.position;
              widget.onPointerButton(false, true);
            }
          },
          onPointerUp: (e) {
            _panning = false;
            _splitting = false;
            widget.onPointerButton(false, false);
          },
          onPointerMove: (e) {
            if (!_panning && !_splitting) return;
            final delta = e.position - _lastMousePos;
            _lastMousePos = e.position;

            if (_panning) {
              widget.onPan(delta);
            }

            if (_splitting && widget.layout.mode == LayoutMode.splitScreen) {
              final box = context.findRenderObject() as RenderBox;
              final localX = e.localPosition.dx;
              widget.onSplit(localX / box.size.width);
            }
          },
          onPointerSignal: (e) {
            if (e is PointerScrollEvent) {
              widget.onZoom(e.scrollDelta.dy, e.localPosition);
            }
          },
          onPointerPanZoomUpdate: (e) {
            // Trackpad two-finger scroll → zoom
            final panDelta = e.pan.dy;
            if (panDelta != 0.0) {
              widget.onZoom(panDelta, e.localPosition);
            }
          },
          child: Texture(textureId: widget.textureId!),
        );
      },
    );
  }
}

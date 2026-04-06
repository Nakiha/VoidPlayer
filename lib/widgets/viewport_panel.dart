import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import '../video_renderer_controller.dart';

/// Three-state viewport matching PySide6 ViewportPanel.
/// States: 0=loading, 1=empty, 2=active(Texture with mouse interaction).
class ViewportPanel extends StatefulWidget {
  final int? textureId;
  final int viewportState; // 0=loading, 1=empty, 2=active
  final String? errorText;
  final LayoutState layout;
  final double renderWidth;
  final double renderHeight;

  final void Function(Offset delta) onPan;
  final void Function(double normalizedX) onSplit;
  final void Function(double delta) onZoom;
  final void Function(bool panning, bool splitting) onPointerButton;

  const ViewportPanel({
    super.key,
    required this.textureId,
    required this.viewportState,
    this.errorText,
    required this.layout,
    this.renderWidth = 1920.0,
    this.renderHeight = 1080.0,
    required this.onPan,
    required this.onSplit,
    required this.onZoom,
    required this.onPointerButton,
  });

  @override
  State<ViewportPanel> createState() => _ViewportPanelState();
}

class _ViewportPanelState extends State<ViewportPanel> {
  bool _panning = false;
  bool _splitting = false;
  Offset _lastMousePos = Offset.zero;

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
              Text('Initializing...',
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
              Text('Click "Add Media" or drag files here',
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
          final scrollDelta = e.scrollDelta.dy;
          widget.onZoom(scrollDelta);
        }
      },
      child: Texture(textureId: widget.textureId!),
    );
  }
}

import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';
import '../video_renderer_controller.dart';
import 'drag_excess_tracker.dart';
import 'track_content.dart';

/// Drag handle with hover highlight and grab cursor.
class _DragHandle extends StatefulWidget {
  final int index;
  const _DragHandle({required this.index});

  @override
  State<_DragHandle> createState() => _DragHandleState();
}

class _DragHandleState extends State<_DragHandle> {
  bool _hovering = false;

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return MouseRegion(
      cursor: SystemMouseCursors.grab,
      onEnter: (_) => setState(() => _hovering = true),
      onExit: (_) => setState(() => _hovering = false),
      child: ReorderableDragStartListener(
        index: widget.index,
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 150),
          width: 28,
          height: 40,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            color: _hovering
                ? colorScheme.onSurface.withValues(alpha: 0.08)
                : Colors.transparent,
            borderRadius: BorderRadius.circular(4),
          ),
          child: Icon(
            Icons.drag_handle,
            size: 16,
            color: _hovering
                ? colorScheme.onSurface
                : colorScheme.onSurfaceVariant,
          ),
        ),
      ),
    );
  }
}

/// Vertical divider with drag-to-resize, hover highlight, and excess tracking.
///
/// When the splitter hits the min/max boundary, the "overshoot" distance is
/// recorded. Dragging back must first consume this overshoot before the
/// splitter resumes moving — so the mouse visually "catches up" to the handle.
class _ResizableDivider extends StatefulWidget {
  final Color color;
  final double controlsWidth;
  final ValueChanged<double> onWidthChanged;

  const _ResizableDivider({
    required this.color,
    required this.controlsWidth,
    required this.onWidthChanged,
  });

  @override
  State<_ResizableDivider> createState() => _ResizableDividerState();
}

class _ResizableDividerState extends State<_ResizableDivider> {
  bool _hovering = false;
  final _dragTracker = DragExcessTracker();
  late double _effectiveWidth;

  @override
  void initState() {
    super.initState();
    _effectiveWidth = widget.controlsWidth;
  }

  @override
  void didUpdateWidget(covariant _ResizableDivider oldWidget) {
    super.didUpdateWidget(oldWidget);
    _effectiveWidth = widget.controlsWidth;
    _dragTracker.sync(_effectiveWidth);
  }

  void _onDragStart(_) {
    _effectiveWidth = widget.controlsWidth;
    _dragTracker.start(_effectiveWidth);
  }

  void _onDragUpdate(DragUpdateDetails details) {
    const minW = 160.0, maxW = 600.0;
    _effectiveWidth = _dragTracker.update(
      delta: details.delta.dx,
      min: minW,
      max: maxW,
    );
    widget.onWidthChanged(_effectiveWidth);
  }

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onHorizontalDragStart: _onDragStart,
      onHorizontalDragUpdate: _onDragUpdate,
      child: MouseRegion(
        cursor: SystemMouseCursors.resizeColumn,
        onEnter: (_) => setState(() => _hovering = true),
        onExit: (_) => setState(() => _hovering = false),
        child: SizedBox.expand(
          child: Center(
            child: Container(
              width: _hovering ? 2 : 0,
              color: _hovering
                  ? Theme.of(context).colorScheme.primary
                  : Colors.transparent,
            ),
          ),
        ),
      ),
    );
  }
}

/// Single track row matching PySide6 TrackRow (40px height).
/// Horizontal split: drag handle + controls + track content (expanded).
class TrackRow extends StatelessWidget {
  final TrackInfo track;
  final int index; // display position index for ReorderableListView
  final double playheadPosition; // per-track clamped 0.0 - 1.0
  final double durationRatio; // clip width ratio (original / max effective)
  final double offsetRatio; // clip start offset ratio
  final VoidCallback onRemove;
  final ValueChanged<int> onOffsetChanged;
  final int syncOffsetMs;
  final double controlsWidth;
  final ValueChanged<double> onControlsWidthChanged;
  final int hoverPtsUs;
  final bool sliderHovering;
  final int trackDurationUs;
  final int offsetUs;
  final int maxEffectiveDurationUs;
  final List<int> markerPtsUs;
  final bool loopRangeEnabled;
  final int loopStartUs;
  final int loopEndUs;

  const TrackRow({
    super.key,
    required this.track,
    required this.index,
    this.playheadPosition = 0.0,
    this.durationRatio = 1.0,
    this.offsetRatio = 0.0,
    required this.onRemove,
    required this.onOffsetChanged,
    this.syncOffsetMs = 0,
    this.controlsWidth = 320,
    required this.onControlsWidthChanged,
    this.hoverPtsUs = 0,
    this.sliderHovering = false,
    this.trackDurationUs = 0,
    this.offsetUs = 0,
    this.maxEffectiveDurationUs = 0,
    this.markerPtsUs = const [],
    this.loopRangeEnabled = false,
    this.loopStartUs = 0,
    this.loopEndUs = 0,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    // Divider sits at: dragHandle(28) + controlsWidth
    final dividerX = 28.0 + controlsWidth;

    return SizedBox(
      height: 40,
      child: Stack(
        children: [
          // Main row layout
          Row(
            children: [
              // Drag handle with hover highlight
              _DragHandle(index: index),
              // Controls panel
              SizedBox(
                width: controlsWidth,
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 4),
                  child: Row(
                    children: [
                      const SizedBox(width: 4),
                      // File name
                      Expanded(
                        child: Text(
                          _fileName(track.path),
                          style: Theme.of(context).textTheme.bodySmall,
                          overflow: TextOverflow.ellipsis,
                          maxLines: 1,
                        ),
                      ),
                      const SizedBox(width: 4),
                      // Offset controls
                      SizedBox(
                        width: 24,
                        height: 24,
                        child: IconButton(
                          onPressed: () => onOffsetChanged(-10),
                          icon: const Icon(Icons.remove, size: 14),
                          padding: EdgeInsets.zero,
                          constraints: const BoxConstraints.tightFor(
                            width: 24,
                            height: 24,
                          ),
                          tooltip: AppLocalizations.of(context)!.offsetBackward,
                        ),
                      ),
                      _OffsetField(
                        valueMs: syncOffsetMs,
                        onChanged: onOffsetChanged,
                      ),
                      SizedBox(
                        width: 24,
                        height: 24,
                        child: IconButton(
                          onPressed: () => onOffsetChanged(10),
                          icon: const Icon(Icons.add, size: 14),
                          padding: EdgeInsets.zero,
                          constraints: const BoxConstraints.tightFor(
                            width: 24,
                            height: 24,
                          ),
                          tooltip: AppLocalizations.of(context)!.offsetForward,
                        ),
                      ),
                      // Remove button
                      SizedBox(
                        width: 28,
                        height: 28,
                        child: IconButton(
                          onPressed: onRemove,
                          icon: Icon(
                            Icons.close,
                            size: 16,
                            color: colorScheme.error,
                          ),
                          padding: EdgeInsets.zero,
                          constraints: const BoxConstraints.tightFor(
                            width: 28,
                            height: 28,
                          ),
                          tooltip: AppLocalizations.of(context)!.removeTrack,
                        ),
                      ),
                    ],
                  ),
                ),
              ),
              // Visual-only divider line (1px)
              Container(width: 1, color: colorScheme.outlineVariant),
              // Track content (expanded)
              Expanded(
                child: TrackContent(
                  durationRatio: durationRatio,
                  offsetRatio: offsetRatio,
                  playheadPosition: playheadPosition,
                  clipColor: _trackColor(track.slot),
                  hoverPtsUs: hoverPtsUs,
                  sliderHovering: sliderHovering,
                  trackDurationUs: trackDurationUs,
                  offsetUs: offsetUs,
                  maxEffectiveDurationUs: maxEffectiveDurationUs,
                  markerPtsUs: markerPtsUs,
                  loopRangeEnabled: loopRangeEnabled,
                  loopStartUs: loopStartUs,
                  loopEndUs: loopEndUs,
                ),
              ),
            ],
          ),
          // Wider invisible hit area overlay for the divider
          Positioned(
            left: dividerX - 4,
            top: 0,
            bottom: 0,
            width: 9,
            child: _ResizableDivider(
              color: colorScheme.outlineVariant,
              controlsWidth: controlsWidth,
              onWidthChanged: onControlsWidthChanged,
            ),
          ),
        ],
      ),
    );
  }

  String _fileName(String path) {
    final sep = path.contains('/') ? '/' : r'\';
    return path.split(sep).lastOrNull ?? path;
  }

  Color _trackColor(int slot) {
    const colors = [Colors.blue, Colors.orange, Colors.green, Colors.purple];
    return colors[slot % colors.length];
  }
}

/// Editable offset field showing the current sync offset in milliseconds.
/// Displays "+123" / "-45" style text. Click to edit directly.
/// Uses a single TextField with FocusNode to avoid layout jumps between states.
class _OffsetField extends StatefulWidget {
  final int valueMs;
  final ValueChanged<int> onChanged;

  const _OffsetField({required this.valueMs, required this.onChanged});

  @override
  State<_OffsetField> createState() => _OffsetFieldState();
}

class _OffsetFieldState extends State<_OffsetField> {
  late TextEditingController _controller;
  final _focusNode = FocusNode();
  bool _editing = false;

  @override
  void initState() {
    super.initState();
    _controller = TextEditingController(text: _displayText());
    _focusNode.addListener(_onFocusChange);
  }

  String _displayText() => '${widget.valueMs >= 0 ? '+' : ''}${widget.valueMs}';

  @override
  void didUpdateWidget(covariant _OffsetField oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!_editing) {
      _controller.text = _displayText();
    }
  }

  @override
  void dispose() {
    _focusNode.removeListener(_onFocusChange);
    _focusNode.dispose();
    _controller.dispose();
    super.dispose();
  }

  void _onFocusChange() {
    if (_focusNode.hasFocus && !_editing) {
      setState(() {
        _editing = true;
        _controller.text = _displayText();
      });
      // Select all text after the frame builds
      WidgetsBinding.instance.addPostFrameCallback((_) {
        _controller.selection = TextSelection(
          baseOffset: 0,
          extentOffset: _controller.text.length,
        );
      });
    } else if (!_focusNode.hasFocus && _editing) {
      _commitEdit();
    }
  }

  void _commitEdit() {
    final parsed = int.tryParse(_controller.text);
    setState(() {
      _editing = false;
      _controller.text = _displayText();
    });
    if (parsed != null && parsed != widget.valueMs) {
      widget.onChanged(parsed - widget.valueMs);
    }
  }

  @override
  Widget build(BuildContext context) {
    final textStyle = Theme.of(context).textTheme.labelSmall!;
    final border = OutlineInputBorder(
      borderRadius: BorderRadius.circular(4),
      borderSide: BorderSide(
        color: Theme.of(context).colorScheme.outline.withValues(alpha: 0.3),
        width: 0.5,
      ),
    );
    return IntrinsicWidth(
      child: TextField(
        controller: _controller,
        focusNode: _focusNode,
        style: textStyle,
        textAlign: TextAlign.center,
        cursorHeight: textStyle.fontSize,
        decoration: InputDecoration(
          isDense: true,
          contentPadding: const EdgeInsets.symmetric(
            horizontal: 4,
            vertical: 5,
          ),
          border: border,
          enabledBorder: border,
          focusedBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(4),
            borderSide: BorderSide(
              color: Theme.of(context).colorScheme.primary,
              width: 1,
            ),
          ),
        ),
        onSubmitted: (_) => _focusNode.unfocus(),
      ),
    );
  }
}

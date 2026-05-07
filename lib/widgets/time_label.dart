import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

/// Time label displaying current / total in MM:SS.mmm format.
/// Matches PySide6 TimeLabel widget.
class TimeLabel extends StatelessWidget {
  final int currentUs;
  final int totalUs;

  const TimeLabel({super.key, required this.currentUs, required this.totalUs});

  static String formatUs(int us) {
    if (us < 0) us = 0;
    final totalMs = us ~/ 1000;
    final minutes = totalMs ~/ 60000;
    final seconds = (totalMs % 60000) ~/ 1000;
    final millis = totalMs % 1000;
    return '${minutes.toString().padLeft(2, '0')}:'
        '${seconds.toString().padLeft(2, '0')}.'
        '${millis.toString().padLeft(3, '0')}';
  }

  @override
  Widget build(BuildContext context) {
    return Text(
      '${formatUs(currentUs)} / ${formatUs(totalUs)}',
      style: Theme.of(context).textTheme.bodySmall?.copyWith(
        fontFeatures: [const FontFeature.tabularFigures()],
      ),
    );
  }
}

class EditableTimeLabel extends StatefulWidget {
  final int currentUs;
  final int totalUs;
  final int? seekMinUs;
  final int? seekMaxUs;
  final ValueChanged<int> onSeek;

  const EditableTimeLabel({
    super.key,
    required this.currentUs,
    required this.totalUs,
    required this.onSeek,
    this.seekMinUs,
    this.seekMaxUs,
  });

  @override
  State<EditableTimeLabel> createState() => _EditableTimeLabelState();
}

class _EditableTimeLabelState extends State<EditableTimeLabel> {
  static const double _timeTextWidth = 62.0;
  static const double _separatorWidth = 14.0;

  late final TextEditingController _controller;
  final _focusNode = FocusNode();
  bool _editing = false;
  String? _editingStartText;

  @override
  void initState() {
    super.initState();
    _controller = TextEditingController(text: _displayCurrent());
    _focusNode.addListener(_handleFocusChange);
  }

  @override
  void didUpdateWidget(covariant EditableTimeLabel oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!_editing) {
      _controller.text = _displayCurrent();
    }
  }

  @override
  void dispose() {
    _focusNode.removeListener(_handleFocusChange);
    _focusNode.dispose();
    _controller.dispose();
    super.dispose();
  }

  String _displayCurrent() => TimeLabel.formatUs(widget.currentUs);

  void _handleFocusChange() {
    if (!_focusNode.hasFocus && _editing) {
      _commit();
    }
  }

  void _startEditing() {
    if (_editing) return;
    setState(() {
      _editing = true;
      _controller.text = _displayCurrent();
      _editingStartText = _controller.text;
    });
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      _focusNode.requestFocus();
      _controller.selection = TextSelection(
        baseOffset: 0,
        extentOffset: _controller.text.length,
      );
    });
  }

  void _commitAndUnfocus() {
    _commit();
    _focusNode.unfocus();
  }

  void _commit() {
    final submittedText = _controller.text.trim();
    if (submittedText == _editingStartText?.trim()) {
      setState(() {
        _editing = false;
        _editingStartText = null;
        _controller.text = _displayCurrent();
      });
      return;
    }

    final parsedUs = parseEditableTimeUs(_controller.text);
    final seekable = widget.totalUs > 0 && parsedUs != null;
    if (seekable) {
      final targetUs = _clampSeekUs(parsedUs);
      setState(() {
        _editing = false;
        _editingStartText = null;
        _controller.text = TimeLabel.formatUs(targetUs);
      });
      if (targetUs != widget.currentUs) {
        widget.onSeek(targetUs);
      }
      return;
    }

    setState(() {
      _editing = false;
      _editingStartText = null;
      _controller.text = _displayCurrent();
    });
  }

  int _clampSeekUs(int us) {
    final minUs = widget.seekMinUs?.clamp(0, widget.totalUs).toInt() ?? 0;
    final maxUs =
        widget.seekMaxUs?.clamp(minUs, widget.totalUs).toInt() ??
        widget.totalUs;
    return us.clamp(minUs, maxUs).toInt();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final style = theme.textTheme.bodySmall?.copyWith(
      fontFeatures: [const FontFeature.tabularFigures()],
    );

    return DefaultTextStyle.merge(
      style: style,
      child: Row(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.baseline,
        textBaseline: TextBaseline.alphabetic,
        children: [
          SizedBox(
            width: _timeTextWidth,
            child: _editing
                ? TextField(
                    controller: _controller,
                    focusNode: _focusNode,
                    inputFormatters: [
                      const EditableTimeInputFormatter(),
                      LengthLimitingTextInputFormatter(16),
                    ],
                    style: style,
                    textAlign: TextAlign.right,
                    textInputAction: TextInputAction.done,
                    keyboardType: TextInputType.datetime,
                    cursorHeight: style?.fontSize,
                    decoration: InputDecoration(
                      isDense: true,
                      border: UnderlineInputBorder(
                        borderSide: BorderSide(
                          color: theme.colorScheme.primary.withValues(
                            alpha: 0.7,
                          ),
                          width: 1,
                        ),
                      ),
                      enabledBorder: UnderlineInputBorder(
                        borderSide: BorderSide(
                          color: theme.colorScheme.primary.withValues(
                            alpha: 0.7,
                          ),
                          width: 1,
                        ),
                      ),
                      focusedBorder: UnderlineInputBorder(
                        borderSide: BorderSide(
                          color: theme.colorScheme.primary,
                          width: 1,
                        ),
                      ),
                      contentPadding: EdgeInsets.zero,
                    ),
                    onSubmitted: (_) => _commitAndUnfocus(),
                  )
                : GestureDetector(
                    behavior: HitTestBehavior.opaque,
                    onTap: _startEditing,
                    child: Text(
                      _controller.text,
                      textAlign: TextAlign.right,
                      overflow: TextOverflow.clip,
                      softWrap: false,
                    ),
                  ),
          ),
          SizedBox(
            width: _separatorWidth,
            child: Text('/', textAlign: TextAlign.center),
          ),
          SizedBox(
            width: _timeTextWidth,
            child: Text(
              TimeLabel.formatUs(widget.totalUs),
              overflow: TextOverflow.clip,
              softWrap: false,
            ),
          ),
        ],
      ),
    );
  }
}

class EditableTimeInputFormatter extends TextInputFormatter {
  const EditableTimeInputFormatter();

  static final _allowedText = RegExp(r'^[+-]?[0-9:.]*$');

  @override
  TextEditingValue formatEditUpdate(
    TextEditingValue oldValue,
    TextEditingValue newValue,
  ) {
    if (_allowedText.hasMatch(newValue.text)) {
      return newValue.copyWith(composing: TextRange.empty);
    }
    return oldValue.copyWith(composing: TextRange.empty);
  }
}

int? parseEditableTimeUs(String raw) {
  var text = raw.trim();
  if (text.isEmpty) return null;

  var sign = 1;
  if (text.startsWith('+') || text.startsWith('-')) {
    sign = text.startsWith('-') ? -1 : 1;
    text = text.substring(1);
  }
  if (text.isEmpty) return null;

  final parts = text.split(':');
  if (parts.length > 3 || parts.any((part) => part.isEmpty)) {
    return null;
  }

  var totalSeconds = 0.0;
  for (var i = 0; i < parts.length; i++) {
    final isLast = i == parts.length - 1;
    final part = parts[i];
    final value = isLast
        ? double.tryParse(part)
        : int.tryParse(part)?.toDouble();
    if (value == null || !value.isFinite || value < 0) return null;
    if (!isLast && part.contains('.')) return null;
    if (i > 0 && value >= 60) return null;

    final power = parts.length - i - 1;
    totalSeconds += value * math.pow(60, power);
  }

  return (sign * totalSeconds * Duration.microsecondsPerSecond).round();
}

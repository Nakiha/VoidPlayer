import 'dart:math' as math;

import 'package:flutter/material.dart';

class AppMenuCombo<T> extends StatefulWidget {
  final T value;
  final List<T> items;
  final String Function(T value) labelFor;
  final ValueChanged<T> onChanged;
  final double? width;
  final double height;
  final double itemHeight;
  final double maxMenuWidth;
  final EdgeInsetsGeometry buttonPadding;
  final EdgeInsetsGeometry itemPadding;
  final BorderRadius borderRadius;
  final BoxBorder? border;
  final Color? backgroundColor;
  final TextStyle? textStyle;
  final TextStyle? menuTextStyle;
  final double iconSize;

  const AppMenuCombo({
    super.key,
    required this.value,
    required this.items,
    required this.labelFor,
    required this.onChanged,
    this.width,
    this.height = 32,
    this.itemHeight = 36,
    this.maxMenuWidth = 520,
    this.buttonPadding = const EdgeInsets.symmetric(horizontal: 8),
    this.itemPadding = const EdgeInsets.only(left: 12, right: 16),
    this.borderRadius = const BorderRadius.all(Radius.circular(6)),
    this.border,
    this.backgroundColor,
    this.textStyle,
    this.menuTextStyle,
    this.iconSize = 18,
  });

  @override
  State<AppMenuCombo<T>> createState() => _AppMenuComboState<T>();
}

class _AppMenuComboState<T> extends State<AppMenuCombo<T>>
    with SingleTickerProviderStateMixin {
  final _anchorKey = GlobalKey();
  OverlayEntry? _overlayEntry;
  late final AnimationController _animationController;
  late final Animation<double> _opacity;

  static const _menuGap = 4.0;
  static const _viewPadding = 8.0;
  static const _menuVerticalPadding = 4.0;
  static const _leadingWidth = 20.0;
  static const _leadingGap = 8.0;

  bool get _isOpen => _overlayEntry != null;

  @override
  void initState() {
    super.initState();
    _animationController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 120),
      reverseDuration: const Duration(milliseconds: 90),
    );
    _opacity = CurvedAnimation(
      parent: _animationController,
      curve: Curves.easeOutCubic,
      reverseCurve: Curves.easeInCubic,
    );
  }

  @override
  void didUpdateWidget(covariant AppMenuCombo<T> oldWidget) {
    super.didUpdateWidget(oldWidget);
    _overlayEntry?.markNeedsBuild();
  }

  @override
  void dispose() {
    _overlayEntry?.remove();
    _overlayEntry = null;
    _animationController.dispose();
    super.dispose();
  }

  void _toggleMenu() {
    if (_isOpen) {
      _closeMenu();
    } else {
      _openMenu();
    }
  }

  void _openMenu() {
    final overlay = Overlay.of(context);
    if (_overlayEntry != null) return;

    _overlayEntry = OverlayEntry(builder: _buildOverlay);
    overlay.insert(_overlayEntry!);
    setState(() {});
    _animationController.forward(from: 0);
  }

  Future<void> _closeMenu({VoidCallback? onClosed}) async {
    final entry = _overlayEntry;
    if (entry == null) {
      onClosed?.call();
      return;
    }
    try {
      await _animationController.reverse();
    } finally {
      if (_overlayEntry == entry) {
        entry.remove();
        _overlayEntry = null;
        if (mounted) setState(() {});
      }
      onClosed?.call();
    }
  }

  Widget _buildOverlay(BuildContext overlayContext) {
    final geometry = _menuGeometry(overlayContext);
    if (geometry == null) return const SizedBox.shrink();

    return Stack(
      children: [
        Positioned.fill(
          child: GestureDetector(
            behavior: HitTestBehavior.translucent,
            onTap: _closeMenu,
          ),
        ),
        Positioned(
          left: geometry.left,
          top: geometry.top,
          width: geometry.width,
          child: FadeTransition(
            opacity: _opacity,
            child: _MenuSurface<T>(
              value: widget.value,
              items: widget.items,
              labelFor: widget.labelFor,
              width: geometry.width,
              maxHeight: geometry.maxHeight,
              itemHeight: widget.itemHeight,
              itemPadding: widget.itemPadding,
              textStyle: widget.menuTextStyle,
              onSelected: (item) {
                _closeMenu(
                  onClosed: () {
                    if (mounted && item != widget.value) {
                      widget.onChanged(item);
                    }
                  },
                );
              },
            ),
          ),
        ),
      ],
    );
  }

  _MenuGeometry? _menuGeometry(BuildContext overlayContext) {
    final anchorBox =
        _anchorKey.currentContext?.findRenderObject() as RenderBox?;
    final overlayBox =
        Overlay.of(overlayContext).context.findRenderObject() as RenderBox?;
    if (anchorBox == null ||
        overlayBox == null ||
        !anchorBox.hasSize ||
        !overlayBox.hasSize) {
      return null;
    }

    final anchorRect = MatrixUtils.transformRect(
      anchorBox.getTransformTo(overlayBox),
      Offset.zero & anchorBox.size,
    );
    final overlaySize = overlayBox.size;
    final textDirection = Directionality.of(context);
    final textStyle =
        widget.menuTextStyle ??
        widget.textStyle ??
        Theme.of(context).textTheme.bodySmall ??
        const TextStyle();
    final measuredWidth = _measureMenuWidth(textStyle, textDirection);
    final maxWidth = math.max(
      anchorRect.width,
      math.min(widget.maxMenuWidth, overlaySize.width - _viewPadding * 2),
    );
    final width = math
        .max(anchorRect.width, measuredWidth)
        .clamp(anchorRect.width, maxWidth);

    final desiredHeight =
        widget.items.length * widget.itemHeight + _menuVerticalPadding * 2;
    final bottomSpace = overlaySize.height - anchorRect.bottom - _menuGap;
    final topSpace = anchorRect.top - _menuGap;
    final opensUpward = bottomSpace < desiredHeight && topSpace > bottomSpace;
    final availableHeight =
        (opensUpward ? topSpace : bottomSpace) - _viewPadding;
    final maxHeight = desiredHeight.clamp(
      widget.itemHeight,
      math.max(widget.itemHeight, availableHeight),
    );

    final left = anchorRect.left.clamp(
      0.0,
      math.max(0.0, overlaySize.width - width),
    );
    final top = opensUpward
        ? (anchorRect.top - _menuGap - maxHeight).clamp(
            _viewPadding,
            overlaySize.height - maxHeight - _viewPadding,
          )
        : (anchorRect.bottom + _menuGap).clamp(
            _viewPadding,
            overlaySize.height - maxHeight - _viewPadding,
          );

    return _MenuGeometry(
      left: left.toDouble(),
      top: top.toDouble(),
      width: width.toDouble(),
      maxHeight: maxHeight.toDouble(),
      opensUpward: opensUpward,
    );
  }

  double _measureMenuWidth(TextStyle textStyle, TextDirection textDirection) {
    var width = 0.0;
    for (final item in widget.items) {
      final painter = TextPainter(
        text: TextSpan(text: widget.labelFor(item), style: textStyle),
        textDirection: textDirection,
        maxLines: 1,
      )..layout();
      width = math.max(width, painter.width);
    }
    final resolvedPadding = widget.itemPadding.resolve(textDirection);
    return width +
        resolvedPadding.left +
        resolvedPadding.right +
        _leadingWidth +
        _leadingGap;
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final labelStyle = widget.textStyle ?? theme.textTheme.bodySmall;
    final child = SizedBox(
      key: _anchorKey,
      width: widget.width,
      height: widget.height,
      child: Material(
        color: Colors.transparent,
        borderRadius: widget.borderRadius,
        clipBehavior: Clip.antiAlias,
        child: InkWell(
          onTap: _toggleMenu,
          borderRadius: widget.borderRadius,
          child: DecoratedBox(
            decoration: BoxDecoration(
              color: widget.backgroundColor,
              border: widget.border,
              borderRadius: widget.borderRadius,
            ),
            child: Padding(
              padding: widget.buttonPadding,
              child: Row(
                children: [
                  Expanded(
                    child: Text(
                      widget.labelFor(widget.value),
                      style: labelStyle,
                      overflow: TextOverflow.ellipsis,
                      maxLines: 1,
                    ),
                  ),
                  AnimatedRotation(
                    turns: _isOpen ? 0.5 : 0,
                    duration: const Duration(milliseconds: 120),
                    curve: Curves.easeOutCubic,
                    child: Icon(
                      Icons.arrow_drop_down,
                      size: widget.iconSize,
                      color: theme.iconTheme.color,
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
    return widget.width == null
        ? child
        : SizedBox(width: widget.width, child: child);
  }
}

class _MenuSurface<T> extends StatelessWidget {
  final T value;
  final List<T> items;
  final String Function(T value) labelFor;
  final ValueChanged<T> onSelected;
  final double width;
  final double maxHeight;
  final double itemHeight;
  final EdgeInsetsGeometry itemPadding;
  final TextStyle? textStyle;

  const _MenuSurface({
    required this.value,
    required this.items,
    required this.labelFor,
    required this.onSelected,
    required this.width,
    required this.maxHeight,
    required this.itemHeight,
    required this.itemPadding,
    required this.textStyle,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Material(
      color: theme.colorScheme.surfaceContainerHigh,
      elevation: 3,
      shadowColor: theme.colorScheme.shadow.withValues(alpha: 0.18),
      surfaceTintColor: Colors.transparent,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
      clipBehavior: Clip.antiAlias,
      child: ConstrainedBox(
        constraints: BoxConstraints(maxHeight: maxHeight),
        child: SingleChildScrollView(
          padding: const EdgeInsets.symmetric(vertical: 4),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              for (final item in items)
                _MenuOption<T>(
                  value: item,
                  label: labelFor(item),
                  selected: item == value,
                  width: width,
                  height: itemHeight,
                  padding: itemPadding,
                  textStyle: textStyle,
                  onSelected: onSelected,
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class _MenuOption<T> extends StatelessWidget {
  final T value;
  final String label;
  final bool selected;
  final double width;
  final double height;
  final EdgeInsetsGeometry padding;
  final TextStyle? textStyle;
  final ValueChanged<T> onSelected;

  const _MenuOption({
    required this.value,
    required this.label,
    required this.selected,
    required this.width,
    required this.height,
    required this.padding,
    required this.textStyle,
    required this.onSelected,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return SizedBox(
      width: width,
      height: height,
      child: InkWell(
        onTap: () => onSelected(value),
        child: Padding(
          padding: padding,
          child: Row(
            children: [
              SizedBox(
                width: _AppMenuComboState._leadingWidth,
                child: selected
                    ? Icon(
                        Icons.check,
                        size: 16,
                        color: theme.colorScheme.primary,
                      )
                    : null,
              ),
              const SizedBox(width: _AppMenuComboState._leadingGap),
              Expanded(
                child: Text(
                  label,
                  style: (textStyle ?? theme.textTheme.bodySmall)?.copyWith(
                    color: selected ? theme.colorScheme.primary : null,
                  ),
                  overflow: TextOverflow.ellipsis,
                  maxLines: 1,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _MenuGeometry {
  final double left;
  final double top;
  final double width;
  final double maxHeight;
  final bool opensUpward;

  const _MenuGeometry({
    required this.left,
    required this.top,
    required this.width,
    required this.maxHeight,
    required this.opensUpward,
  });
}

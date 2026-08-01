import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

class AppMenuCombo<T> extends StatefulWidget {
  final T value;
  final List<T> items;
  final String Function(T value) labelFor;
  final ValueChanged<T> onChanged;
  final Widget Function(BuildContext context, T value, bool open)?
  buttonBuilder;
  final Widget Function(
    BuildContext context,
    T value,
    String label,
    bool selected,
  )?
  itemBuilder;
  final double? width;
  final double height;
  final double itemHeight;
  final double? minMenuWidth;
  final double maxMenuWidth;
  final EdgeInsetsGeometry buttonPadding;
  final EdgeInsetsGeometry itemPadding;
  final BorderRadius borderRadius;
  final BoxBorder? border;
  final Color? backgroundColor;
  final TextStyle? textStyle;
  final TextStyle? menuTextStyle;
  final double iconSize;
  final String? buttonLabel;
  final IconData? buttonLeadingIcon;
  final Color? foregroundColor;
  final bool showSelectedCheck;
  final IconData? Function(T value)? iconFor;
  final bool notifyOnReselect;
  final bool enabled;

  const AppMenuCombo({
    super.key,
    required this.value,
    required this.items,
    required this.labelFor,
    required this.onChanged,
    this.buttonBuilder,
    this.itemBuilder,
    this.width,
    this.height = 32,
    this.itemHeight = 36,
    this.minMenuWidth,
    this.maxMenuWidth = 520,
    this.buttonPadding = const EdgeInsets.symmetric(horizontal: 8),
    this.itemPadding = const EdgeInsets.only(left: 12, right: 16),
    this.borderRadius = const BorderRadius.all(Radius.circular(6)),
    this.border,
    this.backgroundColor,
    this.textStyle,
    this.menuTextStyle,
    this.iconSize = 18,
    this.buttonLabel,
    this.buttonLeadingIcon,
    this.foregroundColor,
    this.showSelectedCheck = true,
    this.iconFor,
    this.notifyOnReselect = false,
    this.enabled = true,
  });

  @override
  State<AppMenuCombo<T>> createState() => _AppMenuComboState<T>();
}

class _AppMenuComboState<T> extends State<AppMenuCombo<T>>
    with SingleTickerProviderStateMixin {
  final _anchorKey = GlobalKey();
  final _layerLink = LayerLink();
  final _focusNode = FocusNode(debugLabel: 'AppMenuCombo');
  OverlayEntry? _overlayEntry;
  late final AnimationController _animationController;
  late final Animation<double> _opacity;

  static const _menuGap = 4.0;
  static const _viewPadding = 8.0;
  static const _menuVerticalPadding = 4.0;
  static const _leadingWidth = 20.0;
  static const _leadingGap = 8.0;

  bool get _isOpen => _overlayEntry != null;
  bool _closing = false;
  int? _highlightedIndex;

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
    _focusNode.dispose();
    _animationController.dispose();
    super.dispose();
  }

  void _toggleMenu() {
    if (!widget.enabled) return;
    if (_isOpen) {
      _closeMenu();
    } else {
      _openMenu();
    }
  }

  void _openMenu() {
    if (!widget.enabled) return;
    final overlay = Overlay.of(context);
    if (_overlayEntry != null) return;

    _closing = false;
    _highlightedIndex = widget.items
        .indexOf(widget.value)
        .clamp(0, math.max(0, widget.items.length - 1));
    _overlayEntry = OverlayEntry(builder: _buildOverlay);
    overlay.insert(_overlayEntry!);
    _focusNode.requestFocus();
    setState(() {});
    _animationController.forward(from: 0);
  }

  Future<void> _closeMenu({VoidCallback? onClosed}) async {
    final entry = _overlayEntry;
    if (entry == null) {
      onClosed?.call();
      return;
    }
    if (_closing) return;
    _closing = true;
    try {
      await _animationController.reverse();
    } finally {
      if (_overlayEntry == entry) {
        entry.remove();
        _overlayEntry = null;
        if (mounted) setState(() {});
      }
      _closing = false;
      onClosed?.call();
    }
  }

  KeyEventResult _handleKeyEvent(FocusNode node, KeyEvent event) {
    if (!widget.enabled) return KeyEventResult.ignored;
    if (event is! KeyDownEvent) return KeyEventResult.ignored;
    if (event.logicalKey == LogicalKeyboardKey.escape) {
      if (_isOpen) {
        _closeMenu();
        return KeyEventResult.handled;
      }
      return KeyEventResult.ignored;
    }
    if (event.logicalKey == LogicalKeyboardKey.arrowDown) {
      _moveHighlight(1);
      return KeyEventResult.handled;
    }
    if (event.logicalKey == LogicalKeyboardKey.arrowUp) {
      _moveHighlight(-1);
      return KeyEventResult.handled;
    }
    if (event.logicalKey == LogicalKeyboardKey.enter ||
        event.logicalKey == LogicalKeyboardKey.space) {
      _activateHighlighted();
      return KeyEventResult.handled;
    }
    return KeyEventResult.ignored;
  }

  void _moveHighlight(int delta) {
    if (!_isOpen) {
      _openMenu();
      return;
    }
    if (widget.items.isEmpty) return;
    final current = (_highlightedIndex ?? widget.items.indexOf(widget.value))
        .clamp(0, widget.items.length - 1)
        .toInt();
    _highlightedIndex = (current + delta).clamp(0, widget.items.length - 1);
    _overlayEntry?.markNeedsBuild();
  }

  void _activateHighlighted() {
    if (!_isOpen) {
      _openMenu();
      return;
    }
    final index = _highlightedIndex;
    if (index == null || index < 0 || index >= widget.items.length) return;
    final item = widget.items[index];
    _closeMenu(
      onClosed: () {
        if (mounted && (widget.notifyOnReselect || item != widget.value)) {
          widget.onChanged(item);
        }
      },
    );
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
        CompositedTransformFollower(
          link: _layerLink,
          showWhenUnlinked: false,
          targetAnchor: Alignment.topLeft,
          followerAnchor: Alignment.topLeft,
          offset: geometry.followerOffset,
          child: FadeTransition(
            opacity: _opacity,
            child: SizedBox(
              width: geometry.width,
              child: _MenuSurface<T>(
                value: widget.value,
                items: widget.items,
                labelFor: widget.labelFor,
                highlightedIndex: _highlightedIndex,
                width: geometry.width,
                maxHeight: geometry.maxHeight,
                itemHeight: widget.itemHeight,
                itemPadding: widget.itemPadding,
                textStyle: widget.menuTextStyle,
                showSelectedCheck: widget.showSelectedCheck,
                iconFor: widget.iconFor,
                itemBuilder: widget.itemBuilder,
                onSelected: (item) {
                  _closeMenu(
                    onClosed: () {
                      if (mounted &&
                          (widget.notifyOnReselect || item != widget.value)) {
                        widget.onChanged(item);
                      }
                    },
                  );
                },
              ),
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
    final minWidth = math.max(anchorRect.width, widget.minMenuWidth ?? 0);
    final maxWidth = math.max(
      minWidth,
      math.min(widget.maxMenuWidth, overlaySize.width - _viewPadding * 2),
    );
    final width = math.max(minWidth, measuredWidth).clamp(minWidth, maxWidth);

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
      followerOffset: Offset(
        left.toDouble() - anchorRect.left,
        top.toDouble() - anchorRect.top,
      ),
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
    final hasLeading = widget.showSelectedCheck || widget.iconFor != null;
    return width +
        resolvedPadding.left +
        resolvedPadding.right +
        (hasLeading ? _leadingWidth + _leadingGap : 0);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final labelStyle = (widget.textStyle ?? theme.textTheme.bodySmall)
        ?.copyWith(color: widget.foregroundColor);
    final disabledColor = theme.colorScheme.onSurface.withValues(alpha: 0.38);
    final iconColor = widget.enabled
        ? widget.foregroundColor ?? theme.iconTheme.color
        : disabledColor;
    final effectiveLabelStyle = widget.enabled
        ? labelStyle
        : labelStyle?.copyWith(color: disabledColor);
    final buttonLabel = widget.buttonLabel ?? widget.labelFor(widget.value);
    final buttonContent = widget.buttonBuilder?.call(
      context,
      widget.value,
      _isOpen,
    );
    final child = CompositedTransformTarget(
      link: _layerLink,
      child: SizedBox(
        key: _anchorKey,
        width: widget.width,
        height: widget.height,
        child: Material(
          color: Colors.transparent,
          borderRadius: widget.borderRadius,
          clipBehavior: Clip.antiAlias,
          child: InkWell(
            onTap: widget.enabled ? _toggleMenu : null,
            borderRadius: widget.borderRadius,
            child: DecoratedBox(
              decoration: BoxDecoration(
                color: widget.backgroundColor,
                border: widget.border,
                borderRadius: widget.borderRadius,
              ),
              child: Padding(
                padding: widget.buttonPadding,
                child:
                    buttonContent ??
                    Row(
                      children: [
                        if (widget.buttonLeadingIcon != null) ...[
                          Icon(
                            widget.buttonLeadingIcon,
                            size: widget.iconSize,
                            color: iconColor,
                          ),
                          const SizedBox(width: 8),
                        ],
                        Expanded(
                          child: Text(
                            buttonLabel,
                            style: effectiveLabelStyle,
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
                            color: iconColor,
                          ),
                        ),
                      ],
                    ),
              ),
            ),
          ),
        ),
      ),
    );
    return Focus(
      focusNode: _focusNode,
      onKeyEvent: _handleKeyEvent,
      child: widget.width == null
          ? child
          : SizedBox(width: widget.width, child: child),
    );
  }
}

class AppMenuComboArrow extends StatelessWidget {
  final bool open;
  final double size;
  final Color? color;

  const AppMenuComboArrow({
    super.key,
    required this.open,
    this.size = 18,
    this.color,
  });

  @override
  Widget build(BuildContext context) {
    return AnimatedRotation(
      turns: open ? 0.5 : 0,
      duration: const Duration(milliseconds: 120),
      curve: Curves.easeOutCubic,
      child: Icon(Icons.arrow_drop_down, size: size, color: color),
    );
  }
}

class _MenuSurface<T> extends StatelessWidget {
  final T value;
  final List<T> items;
  final String Function(T value) labelFor;
  final ValueChanged<T> onSelected;
  final int? highlightedIndex;
  final double width;
  final double maxHeight;
  final double itemHeight;
  final EdgeInsetsGeometry itemPadding;
  final TextStyle? textStyle;
  final bool showSelectedCheck;
  final IconData? Function(T value)? iconFor;
  final Widget Function(
    BuildContext context,
    T value,
    String label,
    bool selected,
  )?
  itemBuilder;

  const _MenuSurface({
    required this.value,
    required this.items,
    required this.labelFor,
    required this.onSelected,
    required this.highlightedIndex,
    required this.width,
    required this.maxHeight,
    required this.itemHeight,
    required this.itemPadding,
    required this.textStyle,
    required this.showSelectedCheck,
    required this.iconFor,
    required this.itemBuilder,
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
              for (var i = 0; i < items.length; i++)
                _MenuOption<T>(
                  value: items[i],
                  label: labelFor(items[i]),
                  selected: items[i] == value,
                  highlighted: highlightedIndex == i,
                  width: width,
                  height: itemHeight,
                  padding: itemPadding,
                  textStyle: textStyle,
                  showSelectedCheck: showSelectedCheck,
                  icon: iconFor?.call(items[i]),
                  customChild: itemBuilder?.call(
                    context,
                    items[i],
                    labelFor(items[i]),
                    items[i] == value,
                  ),
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
  final bool highlighted;
  final double width;
  final double height;
  final EdgeInsetsGeometry padding;
  final TextStyle? textStyle;
  final bool showSelectedCheck;
  final IconData? icon;
  final Widget? customChild;
  final ValueChanged<T> onSelected;

  const _MenuOption({
    required this.value,
    required this.label,
    required this.selected,
    required this.highlighted,
    required this.width,
    required this.height,
    required this.padding,
    required this.textStyle,
    required this.showSelectedCheck,
    required this.icon,
    required this.customChild,
    required this.onSelected,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return SizedBox(
      width: width,
      height: height,
      child: Material(
        color: highlighted
            ? theme.colorScheme.onSurface.withValues(alpha: 0.08)
            : Colors.transparent,
        child: InkWell(
          onTap: () => onSelected(value),
          child: Padding(
            padding: padding,
            child:
                customChild ??
                Row(
                  children: [
                    if (showSelectedCheck || icon != null) ...[
                      SizedBox(
                        width: _AppMenuComboState._leadingWidth,
                        child: selected && showSelectedCheck
                            ? Icon(
                                Icons.check,
                                size: 16,
                                color: theme.colorScheme.primary,
                              )
                            : icon == null
                            ? null
                            : Icon(
                                icon,
                                size: 16,
                                color: theme.colorScheme.onSurfaceVariant,
                              ),
                      ),
                      const SizedBox(width: _AppMenuComboState._leadingGap),
                    ],
                    Expanded(
                      child: Text(
                        label,
                        style: (textStyle ?? theme.textTheme.bodySmall)
                            ?.copyWith(
                              color: selected
                                  ? theme.colorScheme.primary
                                  : null,
                            ),
                        overflow: TextOverflow.ellipsis,
                        maxLines: 1,
                      ),
                    ),
                  ],
                ),
          ),
        ),
      ),
    );
  }
}

class _MenuGeometry {
  final Offset followerOffset;
  final double width;
  final double maxHeight;
  final bool opensUpward;

  const _MenuGeometry({
    required this.followerOffset,
    required this.width,
    required this.maxHeight,
    required this.opensUpward,
  });
}

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../l10n/app_localizations.dart';
import '../../theme/app_appearance.dart';
import 'settings_page_style.dart';

class AppearanceSettingsPage extends StatelessWidget {
  const AppearanceSettingsPage({super.key});

  @override
  Widget build(BuildContext context) {
    final controller = AppAppearanceScope.of(context);
    return AnimatedBuilder(
      animation: controller,
      builder: (context, _) {
        return _AppearanceSettingsContent(controller: controller);
      },
    );
  }
}

class _AppearanceSettingsContent extends StatelessWidget {
  final AppAppearanceController controller;

  const _AppearanceSettingsContent({required this.controller});

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return SingleChildScrollView(
      padding: SettingsPageStyle.pagePadding,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          SettingsPageTitle(text: l.appearance),
          SettingsPageStyle.contentGap,
          SettingsComboRow<AppThemePreference>(
            label: l.appearanceMode,
            icon: Icons.contrast,
            value: controller.themePreference,
            items: AppThemePreference.values,
            labelFor: (value) => switch (value) {
              AppThemePreference.system => l.followSystem,
              AppThemePreference.light => l.lightMode,
              AppThemePreference.dark => l.darkMode,
            },
            onChanged: (value) {
              unawaited(controller.setThemePreference(value));
            },
          ),
          SettingsPageStyle.contentGap,
          SettingsComboRow<AppAccentPreference>(
            label: l.themeColor,
            icon: Icons.palette_outlined,
            value: controller.accentPreference,
            items: AppAccentPreference.values,
            labelFor: (value) => switch (value) {
              AppAccentPreference.system => l.followSystem,
              AppAccentPreference.custom => l.custom,
            },
            onChanged: (value) {
              unawaited(controller.setAccentPreference(value));
            },
          ),
          AnimatedSize(
            duration: const Duration(milliseconds: 180),
            curve: Curves.easeOutCubic,
            alignment: Alignment.topCenter,
            child: controller.accentPreference == AppAccentPreference.custom
                ? Padding(
                    padding: const EdgeInsets.only(top: 16),
                    child: _AccentColorPicker(
                      color: controller.customAccentColor,
                      onChanged: (color) {
                        unawaited(controller.setCustomAccentColor(color));
                      },
                    ),
                  )
                : const SizedBox.shrink(),
          ),
        ],
      ),
    );
  }
}

class _AccentColorPicker extends StatefulWidget {
  final Color color;
  final ValueChanged<Color> onChanged;

  const _AccentColorPicker({required this.color, required this.onChanged});

  @override
  State<_AccentColorPicker> createState() => _AccentColorPickerState();
}

class _AccentColorPickerState extends State<_AccentColorPicker> {
  late HSVColor _hsv;
  int? _lastEmittedColorValue;

  static const _swatches = [
    Color(0xFF0078D4),
    Color(0xFF1E88E5),
    Color(0xFF00897B),
    Color(0xFF43A047),
    Color(0xFFF9A825),
    Color(0xFFE53935),
    Color(0xFFD81B60),
    Color(0xFF8E24AA),
  ];

  @override
  void initState() {
    super.initState();
    _hsv = HSVColor.fromColor(widget.color);
  }

  @override
  void didUpdateWidget(covariant _AccentColorPicker oldWidget) {
    super.didUpdateWidget(oldWidget);
    final nextValue = widget.color.toARGB32();
    if (nextValue != oldWidget.color.toARGB32() &&
        nextValue != _lastEmittedColorValue) {
      _hsv = HSVColor.fromColor(widget.color);
    }
  }

  void _emit(HSVColor hsv) {
    final color = hsv.toColor();
    setState(() {
      _hsv = hsv;
      _lastEmittedColorValue = color.toARGB32();
    });
    widget.onChanged(color);
  }

  void _setColor(Color color) {
    final hsv = HSVColor.fromColor(color);
    setState(() {
      _hsv = hsv;
      _lastEmittedColorValue = color.toARGB32();
    });
    widget.onChanged(color);
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final color = _hsv.toColor();
    return SizedBox(
      width: double.infinity,
      child: DecoratedBox(
        decoration: BoxDecoration(
          border: Border.all(color: theme.colorScheme.outlineVariant),
          borderRadius: BorderRadius.circular(8),
        ),
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Text(
                    l.customThemeColor,
                    style: SettingsPageStyle.sectionTitle(context),
                  ),
                  const Spacer(),
                  _HexColorField(color: color, onSubmitted: _setColor),
                ],
              ),
              const SizedBox(height: 12),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  for (final swatch in _swatches)
                    _SwatchButton(
                      color: swatch,
                      selected: swatch.toARGB32() == color.toARGB32(),
                      onPressed: () => _setColor(swatch),
                    ),
                ],
              ),
              const SizedBox(height: 16),
              _ColorSlider(
                label: l.hue,
                value: _hsv.hue,
                max: 360,
                displayValue: '${_hsv.hue.round()}',
                onChanged: (value) {
                  _emit(_hsv.withHue(value));
                },
              ),
              _ColorSlider(
                label: l.saturation,
                value: _hsv.saturation,
                max: 1,
                displayValue: '${(_hsv.saturation * 100).round()}%',
                onChanged: (value) {
                  _emit(_hsv.withSaturation(value));
                },
              ),
              _ColorSlider(
                label: l.brightness,
                value: _hsv.value,
                max: 1,
                displayValue: '${(_hsv.value * 100).round()}%',
                onChanged: (value) {
                  _emit(_hsv.withValue(value));
                },
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _SwatchButton extends StatelessWidget {
  final Color color;
  final bool selected;
  final VoidCallback onPressed;

  const _SwatchButton({
    required this.color,
    required this.selected,
    required this.onPressed,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return SizedBox(
      width: 30,
      height: 30,
      child: Tooltip(
        message:
            '#${color.toARGB32().toRadixString(16).substring(2).toUpperCase()}',
        child: InkResponse(
          onTap: onPressed,
          radius: 18,
          child: DecoratedBox(
            decoration: BoxDecoration(
              color: color,
              shape: BoxShape.circle,
              border: Border.all(
                color: selected
                    ? theme.colorScheme.onSurface
                    : theme.colorScheme.outlineVariant,
                width: selected ? 2 : 1,
              ),
            ),
            child: selected
                ? const Icon(Icons.check, size: 16, color: Colors.white)
                : null,
          ),
        ),
      ),
    );
  }
}

class _HexColorField extends StatefulWidget {
  final Color color;
  final ValueChanged<Color> onSubmitted;

  const _HexColorField({required this.color, required this.onSubmitted});

  @override
  State<_HexColorField> createState() => _HexColorFieldState();
}

class _HexColorFieldState extends State<_HexColorField> {
  late final TextEditingController _controller;
  late final FocusNode _focusNode;
  bool _invalid = false;

  @override
  void initState() {
    super.initState();
    _controller = TextEditingController(text: _format(widget.color));
    _focusNode = FocusNode()..addListener(_onFocusChanged);
  }

  @override
  void didUpdateWidget(covariant _HexColorField oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (_focusNode.hasFocus) return;
    if (widget.color.toARGB32() != oldWidget.color.toARGB32()) {
      _controller.text = _format(widget.color);
      _invalid = false;
    }
  }

  @override
  void dispose() {
    _focusNode.removeListener(_onFocusChanged);
    _focusNode.dispose();
    _controller.dispose();
    super.dispose();
  }

  void _onFocusChanged() {
    if (!_focusNode.hasFocus) {
      _commit();
    }
  }

  void _commit() {
    final parsed = _parse(_controller.text);
    if (parsed == null) {
      setState(() => _invalid = true);
      return;
    }
    setState(() {
      _invalid = false;
      _controller.text = _format(parsed);
    });
    widget.onSubmitted(parsed);
  }

  static String _format(Color color) {
    return '#${color.toARGB32().toRadixString(16).substring(2).toUpperCase()}';
  }

  static Color? _parse(String raw) {
    final text = raw.trim();
    final hex = text.startsWith('#') ? text.substring(1) : text;
    if (!RegExp(r'^[0-9a-fA-F]{6}$').hasMatch(hex)) {
      return null;
    }
    return Color(0xFF000000 | int.parse(hex, radix: 16));
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final foreground = widget.color.computeLuminance() > 0.55
        ? Colors.black
        : Colors.white;
    return SizedBox(
      width: 104,
      height: 32,
      child: TextField(
        controller: _controller,
        focusNode: _focusNode,
        textAlign: TextAlign.center,
        textInputAction: TextInputAction.done,
        inputFormatters: [
          FilteringTextInputFormatter.allow(RegExp(r'[0-9a-fA-F#]')),
          LengthLimitingTextInputFormatter(7),
        ],
        style: TextStyle(
          color: foreground,
          fontSize: 12,
          fontWeight: FontWeight.w600,
        ),
        decoration: InputDecoration(
          filled: true,
          fillColor: widget.color,
          isDense: true,
          contentPadding: const EdgeInsets.symmetric(
            horizontal: 8,
            vertical: 8,
          ),
          border: OutlineInputBorder(
            borderRadius: BorderRadius.circular(6),
            borderSide: BorderSide(
              color: _invalid ? theme.colorScheme.error : Colors.transparent,
            ),
          ),
          enabledBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(6),
            borderSide: BorderSide(
              color: _invalid ? theme.colorScheme.error : Colors.transparent,
            ),
          ),
          focusedBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(6),
            borderSide: BorderSide(
              color: _invalid
                  ? theme.colorScheme.error
                  : theme.colorScheme.primary,
            ),
          ),
        ),
        onSubmitted: (_) => _commit(),
      ),
    );
  }
}

class _ColorSlider extends StatelessWidget {
  final String label;
  final double value;
  final double max;
  final String displayValue;
  final ValueChanged<double> onChanged;

  const _ColorSlider({
    required this.label,
    required this.value,
    required this.max,
    required this.displayValue,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        SizedBox(
          width: 76,
          child: Text(label, style: SettingsPageStyle.body(context)),
        ),
        Expanded(
          child: Slider(
            value: value.clamp(0, max).toDouble(),
            max: max,
            onChanged: onChanged,
          ),
        ),
        SizedBox(
          width: 44,
          child: Text(
            displayValue,
            textAlign: TextAlign.right,
            style: SettingsPageStyle.body(context),
          ),
        ),
      ],
    );
  }
}

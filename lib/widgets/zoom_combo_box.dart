import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';
import 'app_menu_combo.dart';

/// Zoom level dropdown using MenuAnchor for a cleaner Material 3 look.
class ZoomComboBox extends StatelessWidget {
  final double value;
  final ValueChanged<double> onChanged;

  static const List<double> presets = [
    1.0,
    1.25,
    1.5,
    2.0,
    3.0,
    4.0,
    5.0,
    10.0,
  ];

  const ZoomComboBox({super.key, required this.value, required this.onChanged});

  String _label(double v, AppLocalizations l) {
    return '${(v * 100).round()}%';
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);

    return AppMenuCombo<double>(
      width: 76,
      height: 32,
      value: value,
      items: presets,
      labelFor: (v) => _label(v, l),
      onChanged: onChanged,
      textStyle: theme.textTheme.bodySmall,
      menuTextStyle: theme.textTheme.bodySmall,
      maxMenuWidth: 220,
    );
  }
}

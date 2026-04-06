import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';

/// Zoom level dropdown matching PySide6 ZoomComboBox (90px width).
class ZoomComboBox extends StatelessWidget {
  final double value;
  final ValueChanged<double> onChanged;

  static const List<double> presets = [
    0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 5.0, 10.0,
  ];

  const ZoomComboBox({
    super.key,
    required this.value,
    required this.onChanged,
  });

  String _label(double v, AppLocalizations l) {
    if (v == 0) return l.zoomFit;
    return '${(v * 100).round()}%';
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return SizedBox(
      width: 90,
      height: 32,
      child: DropdownButton<double>(
        value: presets.contains(value) ? value : null,
        isDense: true,
        isExpanded: true,
        underline: const SizedBox.shrink(),
        style: Theme.of(context).textTheme.bodySmall,
        items: [
          DropdownMenuItem(value: 0, child: Text(l.zoomFit)),
          ...presets.map((v) => DropdownMenuItem(
                value: v,
                child: Text(_label(v, l)),
              )),
        ],
        onChanged: (v) {
          if (v != null) onChanged(v);
        },
      ),
    );
  }
}

import 'package:flutter/material.dart';

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

  String _label(double v) {
    if (v == 0) return 'Fit';
    return '${(v * 100).round()}%';
  }

  @override
  Widget build(BuildContext context) {
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
          const DropdownMenuItem(value: 0, child: Text('Fit')),
          ...presets.map((v) => DropdownMenuItem(
                value: v,
                child: Text(_label(v)),
              )),
        ],
        onChanged: (v) {
          if (v != null) onChanged(v);
        },
      ),
    );
  }
}

import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';

/// Zoom level dropdown using MenuAnchor for a cleaner Material 3 look.
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
    final theme = Theme.of(context);
    final allValues = [0.0, ...presets];
    final currentValue = allValues.contains(value) ? value : null;

    return MenuAnchor(
      alignmentOffset: const Offset(0, 4),
      style: MenuStyle(
        backgroundColor: WidgetStatePropertyAll(
          theme.colorScheme.surfaceContainerHigh,
        ),
        shape: WidgetStatePropertyAll(
          RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
        ),
        padding: const WidgetStatePropertyAll(EdgeInsets.symmetric(vertical: 4)),
      ),
      menuChildren: allValues.map((v) {
        final selected = v == currentValue;
        return MenuItemButton(
          leadingIcon: selected
              ? Icon(Icons.check, size: 16, color: theme.colorScheme.primary)
              : const SizedBox(width: 16),
          requestFocusOnHover: false,
          style: ButtonStyle(
            padding: WidgetStatePropertyAll(
              EdgeInsets.only(left: selected ? 8.0 : 12.0, right: 16),
            ),
          ),
          onPressed: () => onChanged(v),
          child: Text(
            _label(v, l),
            style: theme.textTheme.bodySmall?.copyWith(
              color: selected ? theme.colorScheme.primary : null,
            ),
          ),
        );
      }).toList(),
      builder: (context, controller, child) {
        return SizedBox(
          width: 90,
          height: 32,
          child: Material(
            color: Colors.transparent,
            child: InkWell(
              onTap: () {
                if (controller.isOpen) {
                  controller.close();
                } else {
                  controller.open();
                }
              },
              borderRadius: BorderRadius.circular(6),
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 8),
                child: Row(
                  children: [
                    Expanded(
                      child: Text(
                        _label(value, l),
                        style: theme.textTheme.bodySmall,
                        overflow: TextOverflow.ellipsis,
                      ),
                    ),
                    Icon(
                      Icons.arrow_drop_down,
                      size: 18,
                      color: theme.iconTheme.color,
                    ),
                  ],
                ),
              ),
            ),
          ),
        );
      },
    );
  }
}

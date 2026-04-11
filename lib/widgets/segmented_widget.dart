import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';

/// Side-by-side / Split-screen toggle matching PySide6 SegmentedWidget (240x32).
class ViewModeSelector extends StatelessWidget {
  final int currentMode; // 0=sideBySide, 1=splitScreen
  final ValueChanged<int> onChanged;

  const ViewModeSelector({
    super.key,
    required this.currentMode,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return SizedBox(
      width: 240,
      height: 32,
      child: DecoratedBox(
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(6),
          border: Border.all(color: colorScheme.outlineVariant),
        ),
        child: Row(
          children: [
            _Segment(
              label: AppLocalizations.of(context)!.sideBySide,
              selected: currentMode == 0,
              borderRadius: const BorderRadius.horizontal(left: Radius.circular(5)),
              onTap: () => onChanged(0),
            ),
            _Segment(
              label: AppLocalizations.of(context)!.splitScreen,
              selected: currentMode == 1,
              borderRadius: const BorderRadius.horizontal(right: Radius.circular(5)),
              onTap: () => onChanged(1),
            ),
          ],
        ),
      ),
    );
  }
}

class _Segment extends StatelessWidget {
  final String label;
  final bool selected;
  final BorderRadius borderRadius;
  final VoidCallback onTap;

  const _Segment({
    required this.label,
    required this.selected,
    required this.borderRadius,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Expanded(
      child: GestureDetector(
        onTap: onTap,
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 150),
          decoration: BoxDecoration(
            color: selected ? colorScheme.primary : Colors.transparent,
            borderRadius: borderRadius,
          ),
          alignment: Alignment.center,
          child: Text(
            label,
            style: Theme.of(context).textTheme.labelLarge?.copyWith(
                  color: selected ? colorScheme.onPrimary : colorScheme.onSurface,
                ),
          ),
        ),
      ),
    );
  }
}

import 'package:flutter/material.dart';

import '../l10n/app_localizations.dart';

/// Side-by-side / Split-screen toggle matching PySide6 SegmentedWidget (240x32).
class ViewModeSelector extends StatelessWidget {
  final int currentMode; // 0=sideBySide, 1=splitScreen
  final ValueChanged<int> onChanged;
  final String? firstLabel;
  final String? secondLabel;
  final double width;
  final double height;
  final bool enabled;
  final FontWeight? labelFontWeight;

  const ViewModeSelector({
    super.key,
    required this.currentMode,
    required this.onChanged,
    this.firstLabel,
    this.secondLabel,
    this.width = 240,
    this.height = 32,
    this.enabled = true,
    this.labelFontWeight,
  });

  @override
  Widget build(BuildContext context) {
    return Opacity(
      opacity: enabled ? 1 : 0.5,
      child: SizedBox(
        width: width,
        height: height,
        child: SegmentedButton<int>(
          showSelectedIcon: false,
          expandedInsets: EdgeInsets.zero,
          segments: [
            ButtonSegment(
              value: 0,
              label: Text(
                firstLabel ?? AppLocalizations.of(context)!.sideBySide,
              ),
            ),
            ButtonSegment(
              value: 1,
              label: Text(
                secondLabel ?? AppLocalizations.of(context)!.splitScreen,
              ),
            ),
          ],
          selected: {currentMode},
          onSelectionChanged: enabled
              ? (selection) => onChanged(selection.first)
              : null,
          style: ButtonStyle(
            visualDensity: VisualDensity.compact,
            tapTargetSize: MaterialTapTargetSize.shrinkWrap,
            textStyle: WidgetStatePropertyAll(
              TextStyle(fontWeight: labelFontWeight),
            ),
            fixedSize: WidgetStatePropertyAll(Size.fromHeight(height)),
          ),
        ),
      ),
    );
  }
}

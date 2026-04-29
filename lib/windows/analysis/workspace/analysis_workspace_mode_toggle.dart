import 'package:flutter/material.dart';

import '../../../l10n/app_localizations.dart';
import '../../../widgets/segmented_widget.dart';
import '../widgets/analysis_style.dart';

class AnalysisWorkspaceModeToggle extends StatelessWidget {
  final bool splitView;
  final bool enabled;
  final ValueChanged<bool> onChanged;

  const AnalysisWorkspaceModeToggle({
    super.key,
    required this.splitView,
    required this.enabled,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return ExcludeSemantics(
      child: ViewModeSelector(
        currentMode: splitView ? 1 : 0,
        onChanged: (value) => onChanged(value == 1),
        firstLabel: l.analysisTabsMode,
        secondLabel: l.analysisSplitMode,
        width: 124,
        height: analysisHeaderControlHeight,
        enabled: enabled,
        labelFontWeight: FontWeight.w700,
      ),
    );
  }
}

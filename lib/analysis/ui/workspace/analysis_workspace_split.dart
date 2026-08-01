import 'package:flutter/material.dart';

import '../../../widgets/axtree_region.dart';
import '../widgets/analysis_split_layout_controller.dart';
import '../widgets/analysis_style.dart';
import 'analysis_workspace_mode_toggle.dart';
import 'analysis_workspace_models.dart';
import 'analysis_workspace_tabs.dart';

class AnalysisSplitView extends StatelessWidget {
  final List<AnalysisWorkspaceEntry> entries;
  final bool splitView;
  final bool modeToggleEnabled;
  final int selectedIndex;
  final AnalysisSplitLayoutController layoutController;
  final ValueChanged<bool> onModeChanged;
  final ValueChanged<int> onSelected;
  final Widget Function(AnalysisWorkspaceEntry entry) contentBuilder;

  const AnalysisSplitView({
    super.key,
    required this.entries,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.selectedIndex,
    required this.layoutController,
    required this.onModeChanged,
    required this.onSelected,
    required this.contentBuilder,
  });

  @override
  Widget build(BuildContext context) {
    final count = entries.length;
    final columns = count <= 2 ? count : 2;
    final rows = (count / columns).ceil();

    return Column(
      children: [
        for (var row = 0; row < rows; row++)
          Expanded(
            child: Row(
              children: [
                for (var col = 0; col < columns; col++)
                  Expanded(child: _splitCell(context, row * columns + col)),
              ],
            ),
          ),
      ],
    );
  }

  Widget _splitCell(BuildContext context, int index) {
    if (index >= entries.length) return const SizedBox.shrink();
    final entry = entries[index];
    return AxTreeRegion(
      label: 'Analysis pane ${index + 1}',
      child: AnalysisTrackPane(
        entry: entry,
        index: index,
        selected: index == selectedIndex,
        showModeToggle: index == 0,
        splitView: splitView,
        modeToggleEnabled: modeToggleEnabled,
        onModeChanged: onModeChanged,
        onSelected: () => onSelected(index),
        child: contentBuilder(entry),
      ),
    );
  }
}

class AnalysisTrackPane extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;
  final int index;
  final bool selected;
  final bool showModeToggle;
  final bool splitView;
  final bool modeToggleEnabled;
  final ValueChanged<bool> onModeChanged;
  final VoidCallback? onSelected;
  final Widget child;

  const AnalysisTrackPane({
    super.key,
    required this.entry,
    required this.index,
    required this.selected,
    required this.showModeToggle,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.onModeChanged,
    required this.child,
    this.onSelected,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Column(
      children: [
        Container(
          height: analysisHeaderHeight,
          padding: analysisHeaderPadding,
          color: selected
              ? theme.colorScheme.primaryContainer.withValues(alpha: 0.18)
              : theme.colorScheme.surfaceContainerLowest,
          child: Row(
            children: [
              if (showModeToggle) ...[
                AnalysisWorkspaceModeToggle(
                  splitView: splitView,
                  enabled: modeToggleEnabled,
                  onChanged: onModeChanged,
                ),
                const SizedBox(width: analysisHeaderGap),
              ],
              Expanded(
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 4),
                  child: AnalysisTrackTitleButton(
                    entry: entry,
                    index: index,
                    selected: selected,
                    onTap: onSelected,
                  ),
                ),
              ),
            ],
          ),
        ),
        Expanded(child: child),
      ],
    );
  }
}

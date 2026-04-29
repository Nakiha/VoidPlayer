import 'package:flutter/material.dart';

import '../page/analysis_page.dart';
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

  const AnalysisSplitView({
    super.key,
    required this.entries,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.selectedIndex,
    required this.layoutController,
    required this.onModeChanged,
    required this.onSelected,
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
                  Expanded(
                    child: _splitCell(
                      context,
                      row * columns + col,
                      row: row,
                      col: col,
                      rows: rows,
                      columns: columns,
                    ),
                  ),
              ],
            ),
          ),
      ],
    );
  }

  Widget _splitCell(
    BuildContext context,
    int index, {
    required int row,
    required int col,
    required int rows,
    required int columns,
  }) {
    if (index >= entries.length) return const SizedBox.shrink();
    final entry = entries[index];
    final theme = Theme.of(context);
    final divider = BorderSide(color: theme.colorScheme.outlineVariant);
    return Container(
      decoration: BoxDecoration(
        border: Border(
          right: col < columns - 1 ? divider : BorderSide.none,
          bottom: row < rows - 1 ? divider : BorderSide.none,
        ),
      ),
      child: AnalysisTrackPane(
        entry: entry,
        index: index,
        selected: index == selectedIndex,
        showModeToggle: index == 0,
        splitView: splitView,
        modeToggleEnabled: modeToggleEnabled,
        onModeChanged: onModeChanged,
        onSelected: () => onSelected(index),
        child: AnalysisPage(
          key: ValueKey('analysis-split-${entry.hash}'),
          hash: entry.hash,
          pollSummary: false,
          splitLayoutController: layoutController,
        ),
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
          decoration: BoxDecoration(
            color: selected
                ? theme.colorScheme.primaryContainer.withValues(alpha: 0.18)
                : theme.colorScheme.surface.withValues(alpha: 0.18),
            border: Border(bottom: BorderSide(color: theme.dividerColor)),
          ),
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
                  padding: const EdgeInsets.symmetric(horizontal: 2),
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

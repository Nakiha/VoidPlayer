import 'package:flutter/material.dart';

import '../widgets/analysis_style.dart';
import 'analysis_workspace_mode_toggle.dart';
import 'analysis_workspace_models.dart';

class AnalysisTabbedView extends StatelessWidget {
  final List<AnalysisWorkspaceEntry> entries;
  final int selectedIndex;
  final bool splitView;
  final bool modeToggleEnabled;
  final ValueChanged<bool> onModeChanged;
  final ValueChanged<int> onSelected;
  final Widget child;

  const AnalysisTabbedView({
    super.key,
    required this.entries,
    required this.selectedIndex,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.onModeChanged,
    required this.onSelected,
    required this.child,
  });

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        AnalysisTabsHeader(
          entries: entries,
          selectedIndex: selectedIndex,
          splitView: splitView,
          modeToggleEnabled: modeToggleEnabled,
          onModeChanged: onModeChanged,
          onSelected: onSelected,
        ),
        Expanded(child: child),
      ],
    );
  }
}

class AnalysisTabsHeader extends StatelessWidget {
  final List<AnalysisWorkspaceEntry> entries;
  final int selectedIndex;
  final bool splitView;
  final bool modeToggleEnabled;
  final ValueChanged<bool> onModeChanged;
  final ValueChanged<int> onSelected;

  const AnalysisTabsHeader({
    super.key,
    required this.entries,
    required this.selectedIndex,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.onModeChanged,
    required this.onSelected,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Container(
      height: analysisHeaderHeight,
      padding: analysisHeaderPadding,
      decoration: BoxDecoration(
        color: theme.colorScheme.primaryContainer.withValues(alpha: 0.18),
        border: Border(bottom: BorderSide(color: theme.dividerColor)),
      ),
      child: Row(
        children: [
          AnalysisWorkspaceModeToggle(
            splitView: splitView,
            enabled: modeToggleEnabled,
            onChanged: onModeChanged,
          ),
          const SizedBox(width: analysisHeaderGap),
          Expanded(
            child: Row(
              children: [
                for (var i = 0; i < entries.length; i++)
                  Expanded(
                    child: AnalysisTrackTab(
                      entry: entries[i],
                      index: i,
                      selected: i == selectedIndex,
                      onTap: () => onSelected(i),
                    ),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class AnalysisTrackTab extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;
  final int index;
  final bool selected;
  final VoidCallback onTap;

  const AnalysisTrackTab({
    super.key,
    required this.entry,
    required this.index,
    required this.selected,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 2),
      child: AnalysisTrackTitleButton(
        entry: entry,
        index: index,
        selected: selected,
        onTap: selected ? null : onTap,
      ),
    );
  }
}

class AnalysisTrackTitleButton extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;
  final int index;
  final bool selected;
  final VoidCallback? onTap;

  const AnalysisTrackTitleButton({
    super.key,
    required this.entry,
    required this.index,
    required this.selected,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return SizedBox(
      height: analysisHeaderControlHeight,
      child: Material(
        color: selected
            ? theme.colorScheme.primary.withValues(alpha: 0.10)
            : Colors.transparent,
        borderRadius: BorderRadius.circular(4),
        clipBehavior: Clip.antiAlias,
        child: InkWell(
          borderRadius: BorderRadius.circular(4),
          onTap: onTap,
          child: Stack(
            children: [
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 10),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: Text(
                    '${index + 1}. ${entry.fileName ?? 'Track ${index + 1}'}',
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    softWrap: false,
                    style: theme.textTheme.titleSmall?.copyWith(
                      color: selected
                          ? theme.colorScheme.onSurface
                          : theme.colorScheme.onSurfaceVariant,
                      fontWeight: selected ? FontWeight.w600 : null,
                    ),
                  ),
                ),
              ),
              if (selected)
                Positioned(
                  left: 10,
                  right: 10,
                  bottom: 2,
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      color: theme.colorScheme.primary.withValues(alpha: 0.52),
                      borderRadius: BorderRadius.circular(1),
                    ),
                    child: const SizedBox(height: 2),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

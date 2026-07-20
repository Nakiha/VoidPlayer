import 'package:flutter/material.dart';

import '../analysis/ui/page/analysis_page_state.dart';
import '../analysis/ui/page/analysis_page_view.dart';
import '../analysis/ui/workspace/analysis_workspace_models.dart';
import '../l10n/app_localizations.dart';
import 'main_window_quality.dart';
import 'main_window_state.dart';
import 'main_window_view_model.dart';

const Key mainWindowAnalysisTrackSelectorKey = ValueKey(
  'main-window-analysis-track-selector',
);
const Key mainWindowAnalysisChartShelfKey = ValueKey(
  'main-window-analysis-chart-shelf',
);
const Key mainWindowAnalysisNaluSidebarKey = ValueKey(
  'main-window-analysis-nalu-sidebar',
);
const Key mainWindowAnalysisViewModeToggleKey = ValueKey(
  'main-window-analysis-view-mode-toggle',
);

class MainWindowAnalysisDock extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;
  final AnalysisPageViewModel analysisModel;
  final AnalysisPageActions analysisActions;
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const MainWindowAnalysisDock({
    super.key,
    required this.entry,
    required this.analysisModel,
    required this.analysisActions,
    required this.model,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    if (model.deck.tab == MainWindowDeckTab.quality) {
      return MainWindowQualityDeck(
        key: ValueKey('quality-${entry.fileId}'),
        model: model,
        actions: actions,
        selectedFileId: entry.fileId,
        showTrackSelector: true,
      );
    }
    return AnalysisChartPanel(model: analysisModel, actions: analysisActions);
  }
}

class MainWindowAnalysisPendingDock extends StatelessWidget {
  final AnalysisWorkspaceEntry? entry;
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const MainWindowAnalysisPendingDock({
    super.key,
    required this.entry,
    required this.model,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    final focusedFileId =
        entry?.fileId ??
        (model.media.tracks.isEmpty ? null : model.media.tracks.first.fileId);
    if (model.deck.tab == MainWindowDeckTab.quality) {
      return MainWindowQualityDeck(
        key: ValueKey('quality-$focusedFileId'),
        model: model,
        actions: actions,
        selectedFileId: focusedFileId,
        showTrackSelector: true,
      );
    }

    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    return Center(
      child: Text(
        entry?.generationStatus?.isError ?? false
            ? l.analysisCacheStatusFailed
            : l.analysisCacheStatusChecking,
        style: theme.textTheme.bodySmall?.copyWith(
          color: theme.colorScheme.onSurfaceVariant,
        ),
      ),
    );
  }
}

class MainWindowAnalysisDeckHeader extends StatelessWidget {
  final String fileName;
  final int selectedAnalysisTab;
  final bool qualitySelected;
  final bool splitView;
  final bool splitEnabled;
  final VoidCallback onReferencePressed;
  final VoidCallback onTrendPressed;
  final VoidCallback onQualityPressed;
  final ValueChanged<bool> onSplitViewChanged;
  final VoidCallback onClose;
  final Key closeButtonKey;

  const MainWindowAnalysisDeckHeader({
    super.key,
    required this.fileName,
    required this.selectedAnalysisTab,
    required this.qualitySelected,
    required this.splitView,
    required this.splitEnabled,
    required this.onReferencePressed,
    required this.onTrendPressed,
    required this.onQualityPressed,
    required this.onSplitViewChanged,
    required this.onClose,
    required this.closeButtonKey,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    return DecoratedBox(
      decoration: BoxDecoration(
        color: theme.colorScheme.surfaceContainerLowest,
        border: Border(
          bottom: BorderSide(color: theme.colorScheme.outlineVariant),
        ),
      ),
      child: SizedBox(
        height: 40,
        child: Row(
          children: [
            const SizedBox(width: 8),
            Expanded(
              flex: 3,
              child: SizedBox(
                height: 32,
                child: SegmentedButton<int>(
                  showSelectedIcon: false,
                  expandedInsets: EdgeInsets.zero,
                  segments: [
                    ButtonSegment(
                      value: 0,
                      label: KeyedSubtree(
                        key: const ValueKey('main-window-deck-tab-analysis'),
                        child: Text(
                          l.analysisRefPyramid,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                    ),
                    ButtonSegment(
                      value: 1,
                      label: Text(
                        l.analysisFrameTrend,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                      ),
                    ),
                    ButtonSegment(
                      value: 2,
                      label: KeyedSubtree(
                        key: const ValueKey('main-window-deck-tab-quality'),
                        child: Text(
                          l.deckQualityTab,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                    ),
                  ],
                  selected: {qualitySelected ? 2 : selectedAnalysisTab},
                  onSelectionChanged: (selection) {
                    switch (selection.first) {
                      case 0:
                        onReferencePressed();
                      case 1:
                        onTrendPressed();
                      case 2:
                        onQualityPressed();
                    }
                  },
                  style: _deckSegmentStyle,
                ),
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              flex: 2,
              key: mainWindowAnalysisViewModeToggleKey,
              child: SizedBox(
                height: 32,
                child: SegmentedButton<bool>(
                  showSelectedIcon: false,
                  expandedInsets: EdgeInsets.zero,
                  segments: [
                    ButtonSegment(
                      value: false,
                      label: Text(
                        l.analysisTabsMode,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                      ),
                    ),
                    ButtonSegment(
                      value: true,
                      label: Text(
                        l.analysisSplitMode,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                      ),
                    ),
                  ],
                  selected: {splitView},
                  onSelectionChanged: splitEnabled
                      ? (selection) => onSplitViewChanged(selection.first)
                      : null,
                  style: _deckSegmentStyle,
                ),
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              flex: 2,
              child: Text(
                fileName,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: theme.textTheme.labelSmall?.copyWith(
                  color: theme.colorScheme.onSurfaceVariant,
                ),
              ),
            ),
            IconButton(
              key: closeButtonKey,
              onPressed: onClose,
              tooltip: MaterialLocalizations.of(context).closeButtonTooltip,
              icon: const Icon(Icons.close),
            ),
          ],
        ),
      ),
    );
  }
}

const ButtonStyle _deckSegmentStyle = ButtonStyle(
  visualDensity: VisualDensity.compact,
  tapTargetSize: MaterialTapTargetSize.shrinkWrap,
  textStyle: WidgetStatePropertyAll(TextStyle(fontSize: 12)),
  fixedSize: WidgetStatePropertyAll(Size.fromHeight(32)),
);

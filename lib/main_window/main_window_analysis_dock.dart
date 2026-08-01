import 'package:flutter/material.dart';

import '../analysis/ui/page/analysis_page_state.dart';
import '../analysis/ui/page/analysis_page_view.dart';
import '../analysis/ui/workspace/analysis_workspace_models.dart';
import '../l10n/app_localizations.dart';
import '../widgets/app_menu_combo.dart';
import 'main_window_quality.dart';
import 'main_window_state.dart';
import 'main_window_view_model.dart';

const Key mainWindowAnalysisTrackSelectorKey = ValueKey(
  'main-window-analysis-track-selector',
);
const Key mainWindowAnalysisHeaderTrackSelectorKey = ValueKey(
  'main-window-analysis-header-track-selector',
);
const Key mainWindowAnalysisChartShelfKey = ValueKey(
  'main-window-analysis-chart-shelf',
);
const Key mainWindowAnalysisNaluSidebarKey = ValueKey(
  'main-window-analysis-nalu-sidebar',
);

class MainWindowAnalysisDock extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;
  final AnalysisPageViewModel analysisModel;
  final AnalysisPageActions analysisActions;
  final MainWindowViewModel model;
  final MainWindowViewActions actions;
  final MainWindowQualitySession qualitySession;

  const MainWindowAnalysisDock({
    super.key,
    required this.entry,
    required this.analysisModel,
    required this.analysisActions,
    required this.model,
    required this.actions,
    required this.qualitySession,
  });

  @override
  Widget build(BuildContext context) {
    if (model.deck.tab == MainWindowDeckTab.quality) {
      return MainWindowQualityDeck(
        key: ValueKey('quality-${entry.fileId}'),
        model: model,
        actions: actions,
        session: qualitySession,
        selectedFileId: entry.fileId,
      );
    }
    return AnalysisChartPanel(model: analysisModel, actions: analysisActions);
  }
}

class MainWindowAnalysisPendingDock extends StatelessWidget {
  final AnalysisWorkspaceEntry? entry;
  final MainWindowViewModel model;
  final MainWindowViewActions actions;
  final MainWindowQualitySession qualitySession;

  const MainWindowAnalysisPendingDock({
    super.key,
    required this.entry,
    required this.model,
    required this.actions,
    required this.qualitySession,
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
        session: qualitySession,
        selectedFileId: focusedFileId,
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
  final List<AnalysisWorkspaceEntry> entries;
  final int selectedIndex;
  final bool showTrackSelector;
  final ValueChanged<int>? onTrackSelected;
  final int selectedAnalysisTab;
  final bool qualitySelected;
  final VoidCallback onReferencePressed;
  final VoidCallback onTrendPressed;
  final VoidCallback onQualityPressed;
  final VoidCallback onClose;
  final Key closeButtonKey;

  const MainWindowAnalysisDeckHeader({
    super.key,
    required this.entries,
    required this.selectedIndex,
    required this.showTrackSelector,
    required this.onTrackSelected,
    required this.selectedAnalysisTab,
    required this.qualitySelected,
    required this.onReferencePressed,
    required this.onTrendPressed,
    required this.onQualityPressed,
    required this.onClose,
    required this.closeButtonKey,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    return ColoredBox(
      color: theme.colorScheme.surfaceContainerLowest,
      child: SizedBox(
        height: 40,
        child: Padding(
          padding: const EdgeInsets.all(4),
          child: Row(
            children: [
              if (showTrackSelector && entries.length > 1) ...[
                Semantics(
                  label: l.track,
                  value: entries[selectedIndex].fileName,
                  child: AppMenuCombo<int>(
                    key: mainWindowAnalysisHeaderTrackSelectorKey,
                    width: 220,
                    height: 32,
                    value: selectedIndex,
                    items: [
                      for (var index = 0; index < entries.length; index++)
                        index,
                    ],
                    labelFor: (index) =>
                        '${index + 1}. ${entries[index].fileName}',
                    onChanged: (index) => onTrackSelected?.call(index),
                    enabled: onTrackSelected != null,
                    textStyle: theme.textTheme.bodySmall,
                    menuTextStyle: theme.textTheme.bodySmall,
                    backgroundColor: theme.colorScheme.surfaceContainerHigh,
                  ),
                ),
                const SizedBox(width: 4),
              ],
              Expanded(
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
              const SizedBox(width: 4),
              SizedBox.square(
                dimension: 32,
                child: IconButton(
                  key: closeButtonKey,
                  onPressed: onClose,
                  padding: EdgeInsets.zero,
                  tooltip: MaterialLocalizations.of(context).closeButtonTooltip,
                  icon: const Icon(Icons.close, size: 18),
                ),
              ),
            ],
          ),
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

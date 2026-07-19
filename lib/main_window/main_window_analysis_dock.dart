import 'package:flutter/material.dart';

import '../analysis/ui/page/analysis_page_state.dart';
import '../analysis/ui/page/analysis_page_view.dart';
import '../analysis/ui/widgets/analysis_controls.dart';
import '../analysis/ui/workspace/analysis_workspace_models.dart';
import '../l10n/app_localizations.dart';
import '../widgets/axtree_region.dart';
import 'main_window_deck.dart';
import 'main_window_quality.dart';
import 'main_window_state.dart';
import 'main_window_view_model.dart';

const Key mainWindowAnalysisTrackSelectorKey = ValueKey(
  'main-window-analysis-track-selector',
);
const Key mainWindowAnalysisChartShelfKey = ValueKey(
  'main-window-analysis-chart-shelf',
);

class MainWindowAnalysisDock extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;
  final List<AnalysisWorkspaceEntry> entries;
  final int selectedIndex;
  final ValueChanged<int> onSelected;
  final AnalysisPageViewModel analysisModel;
  final AnalysisPageActions analysisActions;
  final MainWindowViewModel model;
  final MainWindowViewActions actions;
  final Widget center;

  const MainWindowAnalysisDock({
    super.key,
    required this.entry,
    required this.entries,
    required this.selectedIndex,
    required this.onSelected,
    required this.analysisModel,
    required this.analysisActions,
    required this.model,
    required this.actions,
    required this.center,
  });

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        const dividerWidth = 9.0;
        final maxSidebar = (constraints.maxWidth * 0.38).clamp(240.0, 420.0);
        final sidebarWidth = analysisModel.naluBrowserWidth.clamp(
          240.0,
          maxSidebar,
        );
        return Row(
          children: [
            SizedBox(
              width: sidebarWidth,
              child: _AnalysisNaluSidebar(
                entries: entries,
                selectedIndex: selectedIndex,
                onSelected: onSelected,
                model: analysisModel,
                actions: analysisActions,
              ),
            ),
            SizedBox(
              width: dividerWidth,
              child: ExcludeSemantics(
                child: AnalysisResizableVDivider(
                  position: sidebarWidth,
                  onPositionChanged: analysisActions.onNaluBrowserWidthChanged,
                ),
              ),
            ),
            Expanded(
              child: _AnalysisCenterAndShelf(
                entry: entry,
                analysisModel: analysisModel,
                analysisActions: analysisActions,
                model: model,
                actions: actions,
                center: center,
              ),
            ),
          ],
        );
      },
    );
  }
}

class MainWindowAnalysisPendingDock extends StatelessWidget {
  final AnalysisWorkspaceEntry? entry;
  final List<AnalysisWorkspaceEntry> entries;
  final int selectedIndex;
  final ValueChanged<int> onSelected;
  final MainWindowViewModel model;
  final MainWindowViewActions actions;
  final Widget center;

  const MainWindowAnalysisPendingDock({
    super.key,
    required this.entry,
    required this.entries,
    required this.selectedIndex,
    required this.onSelected,
    required this.model,
    required this.actions,
    required this.center,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    final qualitySelected = model.deck.tab == MainWindowDeckTab.quality;
    final focusedFileId =
        entry?.fileId ??
        (model.media.tracks.isEmpty ? null : model.media.tracks.first.fileId);
    return Row(
      children: [
        SizedBox(
          width: 300,
          child: DecoratedBox(
            decoration: BoxDecoration(
              color: theme.colorScheme.surfaceContainerLowest,
              border: Border(
                right: BorderSide(color: theme.colorScheme.outlineVariant),
              ),
            ),
            child: Column(
              children: [
                SizedBox(
                  height: 42,
                  child: Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 8),
                    child: entries.isEmpty
                        ? Align(
                            alignment: Alignment.centerLeft,
                            child: Text(
                              model.media.tracks.isEmpty
                                  ? 'No focused track'
                                  : model.media.tracks.first.fileName,
                              overflow: TextOverflow.ellipsis,
                            ),
                          )
                        : DropdownButtonHideUnderline(
                            child: DropdownButton<int>(
                              key: mainWindowAnalysisTrackSelectorKey,
                              value: selectedIndex,
                              isExpanded: true,
                              items: [
                                for (
                                  var index = 0;
                                  index < entries.length;
                                  index++
                                )
                                  DropdownMenuItem(
                                    value: index,
                                    child: Text(
                                      '${index + 1}. '
                                      '${entries[index].fileName}',
                                      overflow: TextOverflow.ellipsis,
                                    ),
                                  ),
                              ],
                              onChanged: (index) {
                                if (index != null) onSelected(index);
                              },
                            ),
                          ),
                  ),
                ),
                Divider(height: 1, color: theme.colorScheme.outlineVariant),
                Expanded(
                  child: Center(
                    child: Padding(
                      padding: const EdgeInsets.all(20),
                      child: Text(
                        entry == null
                            ? 'Preparing NAL unit data…'
                            : entry!.generationStatus?.isError ?? false
                            ? l.analysisCacheStatusFailed
                            : l.analysisCacheStatusChecking,
                        textAlign: TextAlign.center,
                        style: theme.textTheme.bodySmall?.copyWith(
                          color: theme.colorScheme.onSurfaceVariant,
                        ),
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
        Expanded(
          child: Column(
            children: [
              SizedBox(
                key: mainWindowAnalysisChartShelfKey,
                height: 220,
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    color: theme.colorScheme.surfaceContainerLowest,
                    border: Border(
                      bottom: BorderSide(
                        color: theme.colorScheme.outlineVariant,
                      ),
                    ),
                  ),
                  child: Column(
                    children: [
                      SizedBox(
                        height: 40,
                        child: Row(
                          children: [
                            const SizedBox(width: 8),
                            _ShelfButton(
                              key: const ValueKey(
                                'main-window-deck-tab-analysis',
                              ),
                              label: l.deckAnalysisTab,
                              selected: !qualitySelected,
                              onPressed: () {
                                if (qualitySelected) {
                                  actions.deck.onTabChanged(
                                    MainWindowDeckTab.analysis,
                                  );
                                }
                              },
                            ),
                            _ShelfButton(
                              key: const ValueKey(
                                'main-window-deck-tab-quality',
                              ),
                              label: l.deckQualityTab,
                              selected: qualitySelected,
                              onPressed: () {
                                if (!qualitySelected) {
                                  actions.deck.onTabChanged(
                                    MainWindowDeckTab.quality,
                                  );
                                }
                              },
                            ),
                            const Spacer(),
                            IconButton(
                              key: mainWindowDeckCollapseButtonKey,
                              onPressed: () => actions.deck.onTabChanged(
                                MainWindowDeckTab.timeline,
                              ),
                              tooltip: MaterialLocalizations.of(
                                context,
                              ).closeButtonTooltip,
                              icon: const Icon(Icons.close),
                            ),
                          ],
                        ),
                      ),
                      Divider(
                        height: 1,
                        color: theme.colorScheme.outlineVariant,
                      ),
                      Expanded(
                        child: qualitySelected
                            ? MainWindowQualityDeck(
                                model: model,
                                actions: actions,
                                selectedFileId: focusedFileId,
                                showTrackSelector: false,
                              )
                            : Center(
                                child: Text(
                                  'Preparing frame analysis…',
                                  style: theme.textTheme.bodySmall?.copyWith(
                                    color: theme.colorScheme.onSurfaceVariant,
                                  ),
                                ),
                              ),
                      ),
                    ],
                  ),
                ),
              ),
              const SizedBox(key: mainWindowDeckResizeHandleKey, height: 1),
              Expanded(child: center),
            ],
          ),
        ),
      ],
    );
  }
}

class _AnalysisNaluSidebar extends StatelessWidget {
  final List<AnalysisWorkspaceEntry> entries;
  final int selectedIndex;
  final ValueChanged<int> onSelected;
  final AnalysisPageViewModel model;
  final AnalysisPageActions actions;

  const _AnalysisNaluSidebar({
    required this.entries,
    required this.selectedIndex,
    required this.onSelected,
    required this.model,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    return AxTreeRegion(
      label: 'Focused track NAL units',
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: theme.colorScheme.surfaceContainerLowest,
          border: Border(
            right: BorderSide(color: theme.colorScheme.outlineVariant),
          ),
        ),
        child: Column(
          children: [
            SizedBox(
              height: 42,
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 8),
                child: DropdownButtonHideUnderline(
                  child: DropdownButton<int>(
                    key: mainWindowAnalysisTrackSelectorKey,
                    value: selectedIndex,
                    isExpanded: true,
                    isDense: true,
                    items: [
                      for (var index = 0; index < entries.length; index++)
                        DropdownMenuItem(
                          value: index,
                          child: Text(
                            '${index + 1}. ${entries[index].fileName}',
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                          ),
                        ),
                    ],
                    onChanged: (index) {
                      if (index != null) onSelected(index);
                    },
                  ),
                ),
              ),
            ),
            Divider(height: 1, color: theme.colorScheme.outlineVariant),
            SizedBox(
              height: 38,
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: AnalysisOrderToggle(
                    ptsOrder: model.ptsOrder,
                    onChanged: actions.onOrderChanged,
                    l: l,
                  ),
                ),
              ),
            ),
            Divider(height: 1, color: theme.colorScheme.outlineVariant),
            Expanded(
              child: AnalysisNaluPanel(model: model, actions: actions),
            ),
          ],
        ),
      ),
    );
  }
}

class _AnalysisCenterAndShelf extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;
  final AnalysisPageViewModel analysisModel;
  final AnalysisPageActions analysisActions;
  final MainWindowViewModel model;
  final MainWindowViewActions actions;
  final Widget center;

  const _AnalysisCenterAndShelf({
    required this.entry,
    required this.analysisModel,
    required this.analysisActions,
    required this.model,
    required this.actions,
    required this.center,
  });

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        const dividerHeight = 9.0;
        final available = constraints.maxHeight;
        final minShelf = available < 520 ? 130.0 : 160.0;
        final maxShelf = (available - 260).clamp(minShelf, 320.0);
        final shelfHeight = (available * analysisModel.topPanelFraction).clamp(
          minShelf,
          maxShelf,
        );
        return Column(
          children: [
            SizedBox(
              key: mainWindowAnalysisChartShelfKey,
              height: shelfHeight,
              child: _AnalysisChartShelf(
                entry: entry,
                analysisModel: analysisModel,
                analysisActions: analysisActions,
                model: model,
                actions: actions,
              ),
            ),
            SizedBox(
              key: mainWindowDeckResizeHandleKey,
              height: dividerHeight,
              child: ExcludeSemantics(
                child: AnalysisResizableHDivider(
                  position: shelfHeight,
                  minPosition: minShelf,
                  maxPosition: maxShelf,
                  onPositionChanged: (value) {
                    if (available <= 0) return;
                    analysisActions.onTopPanelFractionChanged(
                      value / available,
                    );
                  },
                ),
              ),
            ),
            Expanded(child: center),
          ],
        );
      },
    );
  }
}

class _AnalysisChartShelf extends StatelessWidget {
  final AnalysisWorkspaceEntry entry;
  final AnalysisPageViewModel analysisModel;
  final AnalysisPageActions analysisActions;
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const _AnalysisChartShelf({
    required this.entry,
    required this.analysisModel,
    required this.analysisActions,
    required this.model,
    required this.actions,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    final qualitySelected = model.deck.tab == MainWindowDeckTab.quality;
    return AxTreeRegion(
      label: 'Analysis chart shelf',
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: theme.colorScheme.surfaceContainerLowest,
          border: Border(
            bottom: BorderSide(color: theme.colorScheme.outlineVariant),
          ),
        ),
        child: Column(
          children: [
            SizedBox(
              height: 40,
              child: Row(
                children: [
                  const SizedBox(width: 8),
                  _ShelfButton(
                    key: const ValueKey('main-window-deck-tab-analysis'),
                    label: l.analysisRefPyramid,
                    selected:
                        !qualitySelected && analysisModel.selectedTab == 0,
                    onPressed: () {
                      analysisActions.onTabChanged(0);
                      actions.deck.onTabChanged(MainWindowDeckTab.analysis);
                    },
                  ),
                  _ShelfButton(
                    label: l.analysisFrameTrend,
                    selected:
                        !qualitySelected && analysisModel.selectedTab == 1,
                    onPressed: () {
                      analysisActions.onTabChanged(1);
                      actions.deck.onTabChanged(MainWindowDeckTab.analysis);
                    },
                  ),
                  _ShelfButton(
                    key: const ValueKey('main-window-deck-tab-quality'),
                    label: l.deckQualityTab,
                    selected: qualitySelected,
                    onPressed: () =>
                        actions.deck.onTabChanged(MainWindowDeckTab.quality),
                  ),
                  const Spacer(),
                  Text(
                    entry.fileName,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: theme.textTheme.labelSmall?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant,
                    ),
                  ),
                  IconButton(
                    key: mainWindowDeckCollapseButtonKey,
                    onPressed: () =>
                        actions.deck.onTabChanged(MainWindowDeckTab.timeline),
                    tooltip: MaterialLocalizations.of(
                      context,
                    ).closeButtonTooltip,
                    icon: const Icon(Icons.close),
                  ),
                ],
              ),
            ),
            Divider(height: 1, color: theme.colorScheme.outlineVariant),
            Expanded(
              child: qualitySelected
                  ? MainWindowQualityDeck(
                      key: ValueKey('quality-${entry.fileId}'),
                      model: model,
                      actions: actions,
                      selectedFileId: entry.fileId,
                      showTrackSelector: false,
                    )
                  : AnalysisChartPanel(
                      model: analysisModel,
                      actions: analysisActions,
                    ),
            ),
          ],
        ),
      ),
    );
  }
}

class _ShelfButton extends StatelessWidget {
  final String label;
  final bool selected;
  final VoidCallback onPressed;

  const _ShelfButton({
    super.key,
    required this.label,
    required this.selected,
    required this.onPressed,
  });

  @override
  Widget build(BuildContext context) {
    final colors = Theme.of(context).colorScheme;
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 2, vertical: 4),
      child: TextButton(
        onPressed: onPressed,
        style: TextButton.styleFrom(
          foregroundColor: selected
              ? colors.onPrimaryContainer
              : colors.onSurfaceVariant,
          backgroundColor: selected
              ? colors.primaryContainer
              : Colors.transparent,
          minimumSize: const Size(64, 30),
          padding: const EdgeInsets.symmetric(horizontal: 12),
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          visualDensity: VisualDensity.compact,
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(6)),
        ),
        child: Text(label, maxLines: 1, overflow: TextOverflow.ellipsis),
      ),
    );
  }
}

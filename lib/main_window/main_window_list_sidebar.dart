import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../analysis/analysis_manager.dart';
import '../analysis/ui/page/analysis_page_state.dart';
import '../analysis/ui/page/analysis_page_view.dart';
import '../analysis/ui/widgets/analysis_controls.dart';
import '../analysis/ui/workspace/analysis_workspace_models.dart';
import '../l10n/app_localizations.dart';
import '../track_manager.dart';
import '../widgets/axtree_region.dart';
import '../widgets/quick_mark_sidebar.dart';
import 'main_window_analysis_dock.dart';
import 'main_window_selection.dart';
import 'main_window_state.dart';
import 'main_window_view_model.dart';

const Key mainWindowListSidebarKey = ValueKey('main-window-list-sidebar');
const Key mainWindowListMarksTabKey = ValueKey('main-window-list-marks-tab');
const Key mainWindowListTracksTabKey = ValueKey('main-window-list-tracks-tab');
const Key mainWindowListNaluTabKey = ValueKey('main-window-list-nalu-tab');

enum _MainWindowListTab { marks, tracks, nalu }

class MainWindowAnalysisFocus {
  final AnalysisWorkspaceEntry? entry;
  final List<AnalysisWorkspaceEntry> entries;
  final int selectedIndex;
  final ValueChanged<int> onSelected;
  final AnalysisPageViewModel? pageModel;
  final AnalysisPageActions? pageActions;

  const MainWindowAnalysisFocus({
    required this.entry,
    required this.entries,
    required this.selectedIndex,
    required this.onSelected,
    this.pageModel,
    this.pageActions,
  });

  bool get isReady => pageModel != null && pageActions != null;
}

class MainWindowListSidebar extends StatefulWidget {
  final double width;
  final MainWindowViewModel model;
  final MainWindowViewActions actions;
  final ValueListenable<MainWindowAnalysisFocus?> analysisFocus;
  final VoidCallback onClose;

  const MainWindowListSidebar({
    super.key,
    required this.width,
    required this.model,
    required this.actions,
    required this.analysisFocus,
    required this.onClose,
  });

  @override
  State<MainWindowListSidebar> createState() => _MainWindowListSidebarState();
}

class _MainWindowListSidebarState extends State<MainWindowListSidebar> {
  late _MainWindowListTab _tab;
  var _previousNonAnalysisTab = _MainWindowListTab.marks;

  bool get _analysisContext =>
      widget.model.deck.tab != MainWindowDeckTab.timeline;

  @override
  void initState() {
    super.initState();
    _tab = _analysisContext
        ? _MainWindowListTab.nalu
        : _MainWindowListTab.marks;
  }

  @override
  void didUpdateWidget(covariant MainWindowListSidebar oldWidget) {
    super.didUpdateWidget(oldWidget);
    final wasAnalysis = oldWidget.model.deck.tab != MainWindowDeckTab.timeline;
    if (wasAnalysis == _analysisContext) return;
    if (_analysisContext) {
      if (_tab != _MainWindowListTab.nalu) {
        _previousNonAnalysisTab = _tab;
      }
      _tab = _MainWindowListTab.nalu;
    } else {
      _tab = _previousNonAnalysisTab;
    }
  }

  @override
  Widget build(BuildContext context) {
    final colors = Theme.of(context).colorScheme;
    final l = AppLocalizations.of(context)!;
    return SizedBox(
      key: mainWindowListSidebarKey,
      width: widget.width,
      child: ColoredBox(
        color: colors.surface,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            SizedBox(
              height: 40,
              child: DecoratedBox(
                decoration: BoxDecoration(
                  border: Border(
                    bottom: BorderSide(color: colors.outlineVariant),
                  ),
                ),
                child: Row(
                  children: [
                    const SizedBox(width: 6),
                    Expanded(
                      child: SizedBox(
                        height: 32,
                        child: SegmentedButton<_MainWindowListTab>(
                          showSelectedIcon: false,
                          expandedInsets: EdgeInsets.zero,
                          segments: [
                            ButtonSegment(
                              value: _MainWindowListTab.marks,
                              icon: const Icon(Icons.bookmarks_outlined),
                              label: KeyedSubtree(
                                key: mainWindowListMarksTabKey,
                                child: Text(l.mainWindowListMarks),
                              ),
                            ),
                            ButtonSegment(
                              value: _MainWindowListTab.tracks,
                              icon: const Icon(Icons.video_library_outlined),
                              label: KeyedSubtree(
                                key: mainWindowListTracksTabKey,
                                child: Text(l.mainWindowListTracks),
                              ),
                            ),
                            ButtonSegment(
                              value: _MainWindowListTab.nalu,
                              icon: const Icon(Icons.data_object),
                              label: KeyedSubtree(
                                key: mainWindowListNaluTabKey,
                                child: Text(l.mainWindowListNalu),
                              ),
                            ),
                          ],
                          selected: {_tab},
                          onSelectionChanged: (selection) =>
                              _selectTab(selection.first),
                          style: const ButtonStyle(
                            visualDensity: VisualDensity.compact,
                            tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                            textStyle: WidgetStatePropertyAll(
                              TextStyle(fontSize: 11),
                            ),
                            iconSize: WidgetStatePropertyAll(14),
                            padding: WidgetStatePropertyAll(
                              EdgeInsets.symmetric(horizontal: 4),
                            ),
                          ),
                        ),
                      ),
                    ),
                    IconButton(
                      onPressed: widget.onClose,
                      tooltip: MaterialLocalizations.of(
                        context,
                      ).closeButtonTooltip,
                      icon: const Icon(Icons.close, size: 18),
                    ),
                  ],
                ),
              ),
            ),
            Expanded(
              child: switch (_tab) {
                _MainWindowListTab.marks => QuickMarkSidebar(
                  width: widget.width,
                  marks: widget.model.marks,
                  actions: widget.actions.marks,
                  onClose: widget.onClose,
                  showInspector: false,
                ),
                _MainWindowListTab.tracks => _TrackList(
                  tracks: widget.model.media.tracks,
                  selection: widget.model.selection,
                  onSelected: widget.actions.lists?.onTrackSelected,
                ),
                _MainWindowListTab.nalu => _NaluTabContent(
                  focusListenable: widget.analysisFocus,
                ),
              },
            ),
          ],
        ),
      ),
    );
  }

  void _selectTab(_MainWindowListTab tab) {
    if (_tab == tab) return;
    setState(() {
      _tab = tab;
      if (!_analysisContext && tab != _MainWindowListTab.nalu) {
        _previousNonAnalysisTab = tab;
      }
    });
  }
}

class _TrackList extends StatelessWidget {
  final List<TrackEntry> tracks;
  final MainWindowSelection selection;
  final ValueChanged<int?>? onSelected;

  const _TrackList({
    required this.tracks,
    required this.selection,
    required this.onSelected,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    if (tracks.isEmpty) {
      return Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Text(
            l.mainWindowListNoTracks,
            textAlign: TextAlign.center,
            style: Theme.of(context).textTheme.bodyMedium?.copyWith(
              color: Theme.of(context).colorScheme.onSurfaceVariant,
            ),
          ),
        ),
      );
    }
    final selectedFileId = switch (selection) {
      MainWindowTrackSelection(:final track) => track.fileId,
      _ => null,
    };
    return ListView.builder(
      padding: const EdgeInsets.all(6),
      itemCount: tracks.length,
      itemBuilder: (context, index) {
        final entry = tracks[index];
        final info = entry.info;
        final selected = entry.fileId == selectedFileId;
        final technical = [
          if (info.width > 0 && info.height > 0)
            '${info.width} × ${info.height}',
          if (info.codecName.isNotEmpty) info.codecName.toUpperCase(),
        ].join(' · ');
        return Card(
          key: ValueKey('main-window-track-row-${entry.fileId}'),
          margin: const EdgeInsets.only(bottom: 4),
          elevation: 0,
          color: selected
              ? Theme.of(context).colorScheme.secondaryContainer
              : Colors.transparent,
          clipBehavior: Clip.antiAlias,
          child: ListTile(
            dense: true,
            selected: selected,
            leading: CircleAvatar(radius: 15, child: Text('${entry.slot + 1}')),
            title: Text(
              entry.fileName.isEmpty
                  ? l.quickMarkSidebarTrackLabel(entry.slot + 1)
                  : entry.fileName,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
            ),
            subtitle: technical.isEmpty
                ? Text(l.quickMarkSidebarTrackLabel(entry.slot + 1))
                : Text(technical, maxLines: 1, overflow: TextOverflow.ellipsis),
            trailing: const Icon(Icons.chevron_right, size: 18),
            onTap: onSelected == null
                ? null
                : () => onSelected!(selected ? null : entry.fileId),
          ),
        );
      },
    );
  }
}

class _NaluTabContent extends StatelessWidget {
  final ValueListenable<MainWindowAnalysisFocus?> focusListenable;

  const _NaluTabContent({required this.focusListenable});

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<MainWindowAnalysisFocus?>(
      valueListenable: focusListenable,
      builder: (context, focus, _) => AxTreeRegion(
        label: AppLocalizations.of(context)!.mainWindowListNalu,
        child: SizedBox(
          key: mainWindowAnalysisNaluSidebarKey,
          child: _NaluTabBody(focus: focus),
        ),
      ),
    );
  }
}

class _NaluTabBody extends StatelessWidget {
  final MainWindowAnalysisFocus? focus;

  const _NaluTabBody({required this.focus});

  @override
  Widget build(BuildContext context) {
    final current = focus;
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    if (current == null) {
      return _NaluEmptyState(message: l.mainWindowListNoAnalysis);
    }
    final pageModel = current.pageModel;
    final pageActions = current.pageActions;
    return Column(
      children: [
        _AnalysisTrackSelector(focus: current),
        Divider(height: 1, color: theme.colorScheme.outlineVariant),
        if (current.isReady) ...[
          SizedBox(
            height: 38,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
              child: Align(
                alignment: Alignment.centerLeft,
                child: AnalysisOrderToggle(
                  ptsOrder: pageModel!.ptsOrder,
                  onChanged: pageActions!.onOrderChanged,
                  l: l,
                ),
              ),
            ),
          ),
          Divider(height: 1, color: theme.colorScheme.outlineVariant),
          Expanded(
            child: AnalysisNaluPanel(model: pageModel, actions: pageActions),
          ),
        ] else
          Expanded(
            child: _NaluEmptyState(
              message: _pendingStatusText(l, current.entry?.generationStatus),
            ),
          ),
      ],
    );
  }
}

class _AnalysisTrackSelector extends StatelessWidget {
  final MainWindowAnalysisFocus focus;

  const _AnalysisTrackSelector({required this.focus});

  @override
  Widget build(BuildContext context) {
    if (focus.entries.isEmpty) {
      return SizedBox(
        height: 42,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 8),
          child: Align(
            alignment: Alignment.centerLeft,
            child: Text(
              focus.entry?.fileName ?? '',
              overflow: TextOverflow.ellipsis,
            ),
          ),
        ),
      );
    }
    return SizedBox(
      height: 42,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
        child: DropdownMenu<int>(
          key: mainWindowAnalysisTrackSelectorKey,
          initialSelection: focus.selectedIndex,
          expandedInsets: EdgeInsets.zero,
          requestFocusOnTap: false,
          textStyle: Theme.of(context).textTheme.bodySmall,
          inputDecorationTheme: const InputDecorationTheme(
            isDense: true,
            constraints: BoxConstraints.tightFor(height: 34),
            contentPadding: EdgeInsets.symmetric(horizontal: 10),
            border: OutlineInputBorder(),
          ),
          dropdownMenuEntries: [
            for (var index = 0; index < focus.entries.length; index++)
              DropdownMenuEntry(
                value: index,
                label: '${index + 1}. ${focus.entries[index].fileName}',
              ),
          ],
          onSelected: (index) {
            if (index != null) focus.onSelected(index);
          },
        ),
      ),
    );
  }
}

class _NaluEmptyState extends StatelessWidget {
  final String message;

  const _NaluEmptyState({required this.message});

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(20),
        child: Text(
          message,
          textAlign: TextAlign.center,
          style: Theme.of(context).textTheme.bodySmall?.copyWith(
            color: Theme.of(context).colorScheme.onSurfaceVariant,
          ),
        ),
      ),
    );
  }
}

String _pendingStatusText(
  AppLocalizations l,
  AnalysisTrackGenerationStatus? status,
) {
  if (status?.isError ?? false) return l.analysisCacheStatusFailed;
  return switch (status?.status) {
    AnalysisTrackStatus.generating => l.analysisGeneratingFor(status!.fileName),
    AnalysisTrackStatus.loading => l.analysisCacheStatusLoading,
    AnalysisTrackStatus.cached => l.analysisCacheStatusCached,
    AnalysisTrackStatus.idle => l.analysisCacheStatusMissing,
    AnalysisTrackStatus.computingHash || null => l.analysisCacheStatusChecking,
    AnalysisTrackStatus.error => l.analysisCacheStatusFailed,
  };
}

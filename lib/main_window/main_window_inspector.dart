import 'package:flutter/material.dart';

import '../analysis/ui/analysis_ui_selection.dart';
import '../analysis/ui/page/analysis_page_view.dart';
import '../analysis/ui/widgets/analysis_nalu.dart';
import '../l10n/app_localizations.dart';
import '../widgets/quick_mark_sidebar.dart';
import 'main_window_selection.dart';
import 'main_window_view_model.dart';

const Key mainWindowInspectorKey = ValueKey('main-window-inspector');
const Key mainWindowInspectorCloseKey = ValueKey('main-window-inspector-close');

class MainWindowInspector extends StatelessWidget {
  final double width;
  final MainWindowSelection selection;
  final MainWindowMarksVm marks;
  final MainWindowMarksActions markActions;
  final VoidCallback onClose;

  const MainWindowInspector({
    super.key,
    required this.width,
    required this.selection,
    required this.marks,
    required this.markActions,
    required this.onClose,
  });

  @override
  Widget build(BuildContext context) {
    final colors = Theme.of(context).colorScheme;
    return SizedBox(
      key: mainWindowInspectorKey,
      width: width,
      child: ColoredBox(
        color: colors.surface,
        child: SafeArea(
          top: false,
          bottom: false,
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
                      const SizedBox(width: 12),
                      Expanded(
                        child: Text(
                          _title(context),
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: Theme.of(context).textTheme.titleSmall,
                        ),
                      ),
                      IconButton(
                        key: mainWindowInspectorCloseKey,
                        onPressed: onClose,
                        tooltip: MaterialLocalizations.of(
                          context,
                        ).closeButtonTooltip,
                        icon: const Icon(Icons.close, size: 18),
                      ),
                    ],
                  ),
                ),
              ),
              Expanded(child: _content(context)),
            ],
          ),
        ),
      ),
    );
  }

  String _title(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return switch (selection) {
      MainWindowQuickMarkSelection() => l.quickMarkSidebarInspector,
      MainWindowAnalysisSelection(selection: AnalysisNaluSelection()) =>
        l.analysisNaluDetail,
      MainWindowAnalysisSelection() => l.analysisFrameInfo,
      MainWindowNoSelection() => '',
    };
  }

  Widget _content(BuildContext context) {
    return switch (selection) {
      MainWindowQuickMarkSelection(:final mark) => QuickMarkInspector(
        mark: mark,
        trackLabel: _trackLabel(context, mark.fileId),
        actions: markActions,
      ),
      MainWindowAnalysisSelection(
        selection: final AnalysisNaluSelection selected,
      ) =>
        AnalysisNaluDetailView(
          key: analysisNaluDetailPanelKey,
          nalu: selected.nalu,
          frameIdx: selected.frameIndex,
          frameIndexBase: selected.frameIndex ?? 0,
          frames: selected.frame == null ? const [] : [selected.frame!],
          codec: selected.codec,
          l: AppLocalizations.of(context)!,
        ),
      MainWindowAnalysisSelection(
        selection: final AnalysisFrameSelection selected,
      ) =>
        _AnalysisFrameInspector(selection: selected),
      MainWindowNoSelection() => const SizedBox.shrink(),
    };
  }

  String _trackLabel(BuildContext context, int fileId) {
    final track = marks.tracksByFileId[fileId];
    if (track == null) return '#$fileId';
    final name = track.path.split(RegExp(r'[/\\]')).last;
    final label = AppLocalizations.of(
      context,
    )!.quickMarkSidebarTrackLabel(track.slot + 1);
    return '$label${name.isEmpty ? '' : ' · $name'}';
  }
}

class _AnalysisFrameInspector extends StatelessWidget {
  final AnalysisFrameSelection selection;

  const _AnalysisFrameInspector({required this.selection});

  @override
  Widget build(BuildContext context) {
    final frame = selection.frame;
    final labelStyle = Theme.of(context).textTheme.bodySmall?.copyWith(
      color: Theme.of(context).colorScheme.onSurfaceVariant,
    );
    final valueStyle = Theme.of(context).textTheme.bodySmall;
    final rows = <(String, String)>[
      ('Index', '${selection.frameIndex}'),
      ('POC', '${frame.poc}'),
      ('PTS', '${frame.pts}'),
      ('DTS', '${frame.dts}'),
      ('Packet size', '${frame.packetSize}'),
      ('Average QP', '${frame.avgQp}'),
      ('Temporal ID', '${frame.temporalId}'),
      ('Slice type', '${frame.sliceType}'),
      ('Keyframe', frame.keyframe == 0 ? 'No' : 'Yes'),
    ];
    return SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            AppLocalizations.of(context)!.analysisFrameInfo,
            style: Theme.of(context).textTheme.titleSmall,
          ),
          const SizedBox(height: 8),
          for (final row in rows)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 2),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  SizedBox(width: 92, child: Text(row.$1, style: labelStyle)),
                  Expanded(child: Text(row.$2, style: valueStyle)),
                ],
              ),
            ),
        ],
      ),
    );
  }
}

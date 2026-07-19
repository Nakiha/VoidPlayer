import 'package:flutter/material.dart';

import '../l10n/app_localizations.dart';
import '../track_manager.dart';
import '../widgets/quick_mark_sidebar.dart';
import 'main_window_selection.dart';
import 'main_window_view_model.dart';

const Key mainWindowListSidebarKey = ValueKey('main-window-list-sidebar');
const Key mainWindowListMarksTabKey = ValueKey('main-window-list-marks-tab');
const Key mainWindowListTracksTabKey = ValueKey('main-window-list-tracks-tab');

enum _MainWindowListTab { marks, tracks }

class MainWindowListSidebar extends StatefulWidget {
  final double width;
  final MainWindowViewModel model;
  final MainWindowViewActions actions;
  final VoidCallback onClose;

  const MainWindowListSidebar({
    super.key,
    required this.width,
    required this.model,
    required this.actions,
    required this.onClose,
  });

  @override
  State<MainWindowListSidebar> createState() => _MainWindowListSidebarState();
}

class _MainWindowListSidebarState extends State<MainWindowListSidebar> {
  var _tab = _MainWindowListTab.marks;

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
                    _TabButton(
                      key: mainWindowListMarksTabKey,
                      selected: _tab == _MainWindowListTab.marks,
                      icon: Icons.bookmarks_outlined,
                      label: l.mainWindowListMarks,
                      onPressed: () =>
                          setState(() => _tab = _MainWindowListTab.marks),
                    ),
                    _TabButton(
                      key: mainWindowListTracksTabKey,
                      selected: _tab == _MainWindowListTab.tracks,
                      icon: Icons.video_library_outlined,
                      label: l.mainWindowListTracks,
                      onPressed: () =>
                          setState(() => _tab = _MainWindowListTab.tracks),
                    ),
                    const Spacer(),
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
              },
            ),
          ],
        ),
      ),
    );
  }
}

class _TabButton extends StatelessWidget {
  final bool selected;
  final IconData icon;
  final String label;
  final VoidCallback onPressed;

  const _TabButton({
    super.key,
    required this.selected,
    required this.icon,
    required this.label,
    required this.onPressed,
  });

  @override
  Widget build(BuildContext context) {
    final colors = Theme.of(context).colorScheme;
    return TextButton.icon(
      onPressed: onPressed,
      style: TextButton.styleFrom(
        minimumSize: const Size(0, 32),
        padding: const EdgeInsets.symmetric(horizontal: 8),
        foregroundColor: selected ? colors.primary : colors.onSurfaceVariant,
        backgroundColor: selected
            ? colors.primaryContainer.withValues(alpha: 0.5)
            : Colors.transparent,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(6)),
      ),
      icon: Icon(icon, size: 16),
      label: Text(label),
    );
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

import 'dart:io';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../l10n/app_localizations.dart';
import '../marks/quick_mark.dart';
import '../marks/quick_mark_thumbnail.dart';
import '../utils/time_format.dart';
import '../windows/main/main_window_view_model.dart';
import 'app_menu_combo.dart';

class QuickMarkSidebar extends StatefulWidget {
  final MainWindowMarksVm marks;
  final MainWindowMarksActions actions;
  final double width;
  final VoidCallback onClose;

  const QuickMarkSidebar({
    super.key,
    required this.marks,
    required this.actions,
    required this.width,
    required this.onClose,
  });

  @override
  State<QuickMarkSidebar> createState() => _QuickMarkSidebarState();
}

class _QuickMarkSidebarState extends State<QuickMarkSidebar> {
  static const _toolButtonSize = 28.0;
  static const _rowExtent = 62.0;
  static const _colors = [
    Color(0xFFFF3B30),
    Color(0xFFFF9500),
    Color(0xFFFFCC00),
    Color(0xFF34C759),
    Color(0xFF00C7BE),
    Color(0xFF0A84FF),
    Color(0xFFBF5AF2),
    Color(0xFFFFFFFF),
    Color(0xFF111111),
  ];
  static const _fontSizes = [
    10.0,
    12.0,
    14.0,
    16.0,
    18.0,
    20.0,
    24.0,
    28.0,
    32.0,
  ];
  static const _strokeWidths = [1.0, 2.0, 3.0, 5.0, 8.0];

  final _searchController = TextEditingController();
  final _scrollController = ScrollController();
  final Set<int> _selectedMarkIds = {};
  var _scope = _QuickMarkSidebarScope.all;

  @override
  void initState() {
    super.initState();
    _scheduleScrollSelectedIntoView();
  }

  @override
  void didUpdateWidget(covariant QuickMarkSidebar oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.marks.selectedMarkId != widget.marks.selectedMarkId ||
        oldWidget.marks.allMarks != widget.marks.allMarks ||
        oldWidget.marks.visibleMarks != widget.marks.visibleMarks) {
      _scheduleScrollSelectedIntoView();
    }
  }

  @override
  void dispose() {
    _searchController.dispose();
    _scrollController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    final selected = _selectedMark;
    final scopedMarks = _scopedMarks();
    final scopedMarkIds = scopedMarks.map((mark) => mark.id).toSet();
    final selectedScopedIds = _selectedMarkIds.intersection(scopedMarkIds);
    final selectionActive = _selectedMarkIds.isNotEmpty;

    return SizedBox(
      width: widget.width,
      child: DecoratedBox(
        decoration: BoxDecoration(color: colorScheme.surface),
        child: SafeArea(
          top: false,
          bottom: false,
          child: Column(
            children: [
              _buildSearchRow(l),
              _buildListHeader(
                l,
                count: scopedMarks.length,
                selectableCount: scopedMarkIds.length,
                selectedCount: selectedScopedIds.length,
              ),
              Expanded(
                child: scopedMarks.isEmpty
                    ? _EmptyMarksMessage(
                        text: _emptyText(l, searchActive: _query.isNotEmpty),
                      )
                    : ListView.builder(
                        controller: _scrollController,
                        padding: const EdgeInsets.fromLTRB(4, 0, 4, 4),
                        itemCount: scopedMarks.length,
                        itemBuilder: (context, index) {
                          final mark = scopedMarks[index];
                          return _QuickMarkRow(
                            mark: mark,
                            thumbnail: widget.marks.thumbnailsByMarkId[mark.id],
                            selected: mark.id == widget.marks.selectedMarkId,
                            selectionActive: selectionActive,
                            checked: _selectedMarkIds.contains(mark.id),
                            visibleOnCurrentFrame: widget.marks.visibleMarkIds
                                .contains(mark.id),
                            title: _titleFor(l, mark),
                            titleMuted: mark.text.trim().isEmpty,
                            subtitle: _subtitleFor(l, mark),
                            timecode: formatTimePad2(mark.anchor.ptsUs),
                            trackLabel: _trackLabel(mark.fileId),
                            onTap: () => widget.actions.onJumpToMark(mark.id),
                            onCheckedChanged: (checked) =>
                                _setMarkChecked(mark.id, checked),
                          );
                        },
                      ),
              ),
              if (selected != null && !selectionActive)
                _QuickMarkInspector(
                  mark: selected,
                  trackLabel: _trackLabel(selected.fileId),
                  colors: _colors,
                  fontSizes: _fontSizes,
                  strokeWidths: _strokeWidths,
                  onChanged: widget.actions.onMarkChanged,
                  onJump: () => widget.actions.onJumpToMark(selected.id),
                  onFocus: () => widget.actions.onFocusVisibleMark(selected.id),
                  onDelete: () => widget.actions.onMarkDeleted(selected.id),
                  colorLabel: (color) => _colorLabel(l, color),
                ),
            ],
          ),
        ),
      ),
    );
  }

  QuickMark? get _selectedMark {
    final id = widget.marks.selectedMarkId;
    if (id == null) return null;
    for (final mark in widget.marks.allMarks) {
      if (mark.id == id) return mark;
    }
    return null;
  }

  List<QuickMark> _scopedMarks() {
    return _scope == _QuickMarkSidebarScope.current
        ? _filteredMarks(widget.marks.visibleMarks)
        : _filteredMarks(_sortedMarks(widget.marks.allMarks));
  }

  String get _query => _searchController.text.trim().toLowerCase();

  void _setMarkChecked(int markId, bool checked) {
    setState(() {
      final wasEmpty = _selectedMarkIds.isEmpty;
      if (checked) {
        _selectedMarkIds.add(markId);
        if (wasEmpty) widget.actions.onSelectVisibleMark(null);
      } else {
        _selectedMarkIds.remove(markId);
      }
    });
  }

  void _selectAllMarks(Iterable<QuickMark> marks) {
    final ids = marks.map((mark) => mark.id).toSet();
    if (ids.isEmpty) return;
    setState(() {
      final wasEmpty = _selectedMarkIds.isEmpty;
      _selectedMarkIds
        ..clear()
        ..addAll(ids);
      if (wasEmpty) widget.actions.onSelectVisibleMark(null);
    });
  }

  void _cancelSelection() {
    if (_selectedMarkIds.isEmpty) return;
    setState(_selectedMarkIds.clear);
  }

  void _deleteSelectedMarks() {
    if (_selectedMarkIds.isEmpty) return;
    final ids = _selectedMarkIds.toList(growable: false);
    setState(_selectedMarkIds.clear);
    for (final id in ids) {
      widget.actions.onMarkDeleted(id);
    }
  }

  void _scheduleScrollSelectedIntoView() {
    final selectedMarkId = widget.marks.selectedMarkId;
    if (selectedMarkId == null) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      _scrollSelectedIntoView(selectedMarkId);
    });
  }

  void _scrollSelectedIntoView(int markId) {
    if (!_scrollController.hasClients) return;
    final marks = _scopedMarks();
    final index = marks.indexWhere((mark) => mark.id == markId);
    if (index < 0) return;
    final position = _scrollController.position;
    final rowTop = index * _rowExtent;
    final rowBottom = rowTop + _rowExtent;
    final visibleTop = position.pixels;
    final visibleBottom = visibleTop + position.viewportDimension;
    double? target;
    if (rowTop < visibleTop) {
      target = rowTop;
    } else if (rowBottom > visibleBottom) {
      target = rowBottom - position.viewportDimension;
    }
    if (target == null) return;
    final clamped = target.clamp(
      position.minScrollExtent,
      position.maxScrollExtent,
    );
    _scrollController.animateTo(
      clamped,
      duration: const Duration(milliseconds: 140),
      curve: Curves.easeOutCubic,
    );
  }

  Widget _buildSearchRow(AppLocalizations l) {
    final colorScheme = Theme.of(context).colorScheme;
    return DecoratedBox(
      decoration: BoxDecoration(
        color: colorScheme.surfaceContainerLow,
        border: Border(bottom: BorderSide(color: colorScheme.outlineVariant)),
      ),
      child: Padding(
        padding: const EdgeInsets.all(4),
        child: Row(
          children: [
            _ScopeButton(
              selected: _scope == _QuickMarkSidebarScope.current,
              icon: Icons.filter_center_focus,
              tooltip: l.quickMarkSidebarCurrentFrame,
              onPressed: () =>
                  setState(() => _scope = _QuickMarkSidebarScope.current),
            ),
            const SizedBox(width: 2),
            _ScopeButton(
              selected: _scope == _QuickMarkSidebarScope.all,
              icon: Icons.format_list_bulleted,
              tooltip: l.quickMarkSidebarAllMarks,
              onPressed: () =>
                  setState(() => _scope = _QuickMarkSidebarScope.all),
            ),
            const SizedBox(width: 4),
            Expanded(
              child: SizedBox(
                height: 30,
                child: TextField(
                  controller: _searchController,
                  onChanged: (_) => setState(() {}),
                  textInputAction: TextInputAction.search,
                  style: Theme.of(context).textTheme.bodySmall,
                  decoration: InputDecoration(
                    prefixIcon: const Icon(Icons.search, size: 15),
                    prefixIconConstraints: const BoxConstraints.tightFor(
                      width: 28,
                      height: 28,
                    ),
                    suffixIcon: _query.isEmpty
                        ? null
                        : IconButton(
                            onPressed: () {
                              _searchController.clear();
                              setState(() {});
                            },
                            icon: const Icon(Icons.clear, size: 15),
                            padding: EdgeInsets.zero,
                            constraints: const BoxConstraints.tightFor(
                              width: 26,
                              height: 26,
                            ),
                          ),
                    hintText: l.quickMarkSidebarSearchHint,
                    hintStyle: Theme.of(context).textTheme.bodySmall?.copyWith(
                      color: colorScheme.onSurfaceVariant,
                    ),
                    isDense: true,
                    contentPadding: const EdgeInsets.symmetric(
                      horizontal: 6,
                      vertical: 7,
                    ),
                    border: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(5),
                    ),
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildListHeader(
    AppLocalizations l, {
    required int count,
    required int selectableCount,
    required int selectedCount,
  }) {
    final colorScheme = Theme.of(context).colorScheme;
    final title = _scope == _QuickMarkSidebarScope.current
        ? l.quickMarkSidebarCurrentFrame
        : l.quickMarkSidebarAllMarks;
    final scopedMarks = _scopedMarks();
    final hasSelection = selectedCount > 0;
    return SizedBox(
      height: 34,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(6, 4, 4, 2),
        child: Row(
          children: [
            Expanded(
              child: Row(
                children: [
                  Flexible(
                    child: Text(
                      title,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: Theme.of(context).textTheme.labelMedium?.copyWith(
                        color: colorScheme.onSurface,
                        fontWeight: FontWeight.w700,
                      ),
                    ),
                  ),
                  const SizedBox(width: 6),
                  Text(
                    l.quickMarkSidebarTotalCount(count),
                    maxLines: 1,
                    style: Theme.of(context).textTheme.labelSmall?.copyWith(
                      color: colorScheme.onSurfaceVariant,
                    ),
                  ),
                ],
              ),
            ),
            if (hasSelection) ...[
              Text(
                l.quickMarkSidebarSelectedCount(selectedCount),
                maxLines: 1,
                style: Theme.of(context).textTheme.labelSmall?.copyWith(
                  color: colorScheme.onSurfaceVariant,
                  fontWeight: FontWeight.w600,
                ),
              ),
              const SizedBox(width: 8),
            ],
            _HeaderToolButton(
              icon: Icons.select_all,
              tooltip: l.quickMarkSidebarSelectAll,
              onPressed:
                  selectedCount >= selectableCount || selectableCount == 0
                  ? null
                  : () => _selectAllMarks(scopedMarks),
            ),
            const SizedBox(width: 2),
            _HeaderToolButton(
              icon: Icons.close,
              tooltip: l.quickMarkSidebarCancelSelection,
              onPressed: hasSelection ? _cancelSelection : null,
            ),
            const SizedBox(width: 2),
            _HeaderToolButton(
              icon: Icons.delete_outline,
              tooltip: l.quickMarkSidebarDeleteSelected,
              destructive: true,
              onPressed: hasSelection ? _deleteSelectedMarks : null,
            ),
          ],
        ),
      ),
    );
  }

  String _emptyText(AppLocalizations l, {required bool searchActive}) {
    if (searchActive) return l.quickMarkSidebarNoMatches;
    if (_scope == _QuickMarkSidebarScope.current) {
      return l.quickMarkSidebarNoCurrentFrameMarks;
    }
    return l.quickMarkSidebarNoMarks;
  }

  List<QuickMark> _sortedMarks(List<QuickMark> marks) {
    final next = marks.toList(growable: false);
    next.sort((a, b) {
      final time = a.anchor.ptsUs.compareTo(b.anchor.ptsUs);
      if (time != 0) return time;
      return a.id.compareTo(b.id);
    });
    return next;
  }

  List<QuickMark> _filteredMarks(List<QuickMark> marks) {
    final query = _query;
    if (query.isEmpty) return marks;
    final l = AppLocalizations.of(context)!;
    return marks
        .where((mark) {
          final haystack = [
            _titleFor(l, mark),
            _subtitleFor(l, mark),
            formatTimePad2(mark.anchor.ptsUs),
            mark.anchor.ptsUs.toString(),
            _trackLabel(mark.fileId),
            mark.shape == QuickMarkShape.rectangle
                ? l.quickMarkRectangle
                : l.quickMarkArrow,
          ].join(' ').toLowerCase();
          return haystack.contains(query);
        })
        .toList(growable: false);
  }

  String _titleFor(AppLocalizations l, QuickMark mark) {
    final text = mark.text.trim().replaceAll(RegExp(r'\s+'), ' ');
    if (text.isNotEmpty) return text;
    return l.quickMarkSidebarNoAnnotation;
  }

  String _subtitleFor(AppLocalizations l, QuickMark mark) {
    final shape = mark.shape == QuickMarkShape.rectangle
        ? l.quickMarkRectangle
        : l.quickMarkArrow;
    final sync = mark.syncAcrossTracks
        ? l.quickMarkSidebarSynced
        : l.quickMarkSidebarUnsynced;
    final rect = mark.sourceRect;
    final geometry =
        '${(rect.left * 100).round()},${(rect.top * 100).round()} '
        '${(rect.width.abs() * 100).round()}x'
        '${(rect.height.abs() * 100).round()}%';
    return '$shape · $sync · $geometry';
  }

  String _trackLabel(int fileId) {
    final track = widget.marks.tracksByFileId[fileId];
    if (track == null) return '#$fileId';
    final name = track.path.split(RegExp(r'[/\\]')).last;
    final label = AppLocalizations.of(
      context,
    )!.quickMarkSidebarTrackLabel(track.slot + 1);
    return '$label${name.isEmpty ? '' : ' · $name'}';
  }

  String _colorLabel(AppLocalizations l, Color color) {
    if (color == const Color(0xFFFF3B30)) return l.quickMarkColorRed;
    if (color == const Color(0xFFFF9500)) return l.quickMarkColorOrange;
    if (color == const Color(0xFFFFCC00)) return l.quickMarkColorYellow;
    if (color == const Color(0xFF34C759)) return l.quickMarkColorGreen;
    if (color == const Color(0xFF00C7BE)) return l.quickMarkColorCyan;
    if (color == const Color(0xFF0A84FF)) return l.quickMarkColorBlue;
    if (color == const Color(0xFFBF5AF2)) return l.quickMarkColorPurple;
    if (color == const Color(0xFFFFFFFF)) return l.quickMarkColorWhite;
    if (color == const Color(0xFF111111)) return l.quickMarkColorBlack;
    return l.quickMarkColor;
  }
}

enum _QuickMarkSidebarScope { current, all }

class _ScopeButton extends StatelessWidget {
  final bool selected;
  final IconData icon;
  final String tooltip;
  final VoidCallback onPressed;

  const _ScopeButton({
    required this.selected,
    required this.icon,
    required this.tooltip,
    required this.onPressed,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Tooltip(
      message: tooltip,
      child: IconButton(
        onPressed: onPressed,
        icon: Icon(icon, size: 17),
        color: selected ? colorScheme.primary : colorScheme.onSurfaceVariant,
        style: _quickMarkToggleButtonStyle(
          selected,
          colorScheme,
          const Size.square(_QuickMarkSidebarState._toolButtonSize),
        ),
      ),
    );
  }
}

class _HeaderToolButton extends StatelessWidget {
  final IconData icon;
  final String tooltip;
  final bool destructive;
  final VoidCallback? onPressed;

  const _HeaderToolButton({
    required this.icon,
    required this.tooltip,
    this.destructive = false,
    required this.onPressed,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Tooltip(
      message: tooltip,
      child: IconButton(
        onPressed: onPressed,
        icon: Icon(icon, size: 17),
        color: destructive
            ? null
            : onPressed == null
            ? colorScheme.onSurfaceVariant.withValues(alpha: 0.38)
            : colorScheme.onSurfaceVariant,
        style: destructive
            ? _quickMarkDeleteButtonStyle(colorScheme)
            : _quickMarkToggleButtonStyle(
                false,
                colorScheme,
                const Size.square(_QuickMarkSidebarState._toolButtonSize),
              ),
      ),
    );
  }
}

class _EmptyMarksMessage extends StatelessWidget {
  final String text;

  const _EmptyMarksMessage({required this.text});

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Text(
        text,
        style: Theme.of(context).textTheme.labelSmall?.copyWith(
          color: Theme.of(context).colorScheme.onSurfaceVariant,
        ),
      ),
    );
  }
}

class _QuickMarkRow extends StatefulWidget {
  final QuickMark mark;
  final QuickMarkThumbnail? thumbnail;
  final bool selected;
  final bool selectionActive;
  final bool checked;
  final bool visibleOnCurrentFrame;
  final String title;
  final bool titleMuted;
  final String subtitle;
  final String timecode;
  final String trackLabel;
  final VoidCallback onTap;
  final ValueChanged<bool> onCheckedChanged;

  const _QuickMarkRow({
    required this.mark,
    required this.thumbnail,
    required this.selected,
    required this.selectionActive,
    required this.checked,
    required this.visibleOnCurrentFrame,
    required this.title,
    required this.titleMuted,
    required this.subtitle,
    required this.timecode,
    required this.trackLabel,
    required this.onTap,
    required this.onCheckedChanged,
  });

  @override
  State<_QuickMarkRow> createState() => _QuickMarkRowState();
}

class _QuickMarkRowState extends State<_QuickMarkRow> {
  var _hovering = false;

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    final background = widget.selected
        ? colorScheme.primary.withValues(alpha: 0.16)
        : _hovering
        ? colorScheme.surfaceContainerHighest.withValues(
            alpha: colorScheme.brightness == Brightness.dark ? 0.36 : 0.70,
          )
        : colorScheme.surfaceContainerHighest.withValues(
            alpha: colorScheme.brightness == Brightness.dark ? 0.20 : 0.46,
          );
    return Padding(
      padding: const EdgeInsets.only(bottom: 2),
      child: MouseRegion(
        onEnter: (_) => setState(() => _hovering = true),
        onExit: (_) => setState(() => _hovering = false),
        child: Material(
          color: background,
          borderRadius: BorderRadius.circular(4),
          clipBehavior: Clip.antiAlias,
          child: SizedBox(
            height: 60,
            child: Padding(
              padding: const EdgeInsets.all(4),
              child: Row(
                children: [
                  Expanded(
                    child: InkWell(
                      key: ValueKey('quick-mark-sidebar-row-${widget.mark.id}'),
                      onTap: widget.selectionActive
                          ? () => widget.onCheckedChanged(!widget.checked)
                          : widget.onTap,
                      borderRadius: BorderRadius.circular(3),
                      hoverColor: Colors.transparent,
                      focusColor: Colors.transparent,
                      highlightColor: Colors.transparent,
                      splashColor: Colors.transparent,
                      overlayColor: const WidgetStatePropertyAll(
                        Colors.transparent,
                      ),
                      child: Row(
                        children: [
                          _QuickMarkPreview(
                            mark: widget.mark,
                            thumbnail: widget.thumbnail,
                          ),
                          const SizedBox(width: 4),
                          Expanded(
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                              children: [
                                Row(
                                  children: [
                                    Text(
                                      widget.timecode,
                                      style: Theme.of(context)
                                          .textTheme
                                          .labelSmall
                                          ?.copyWith(
                                            fontWeight: FontWeight.w700,
                                          ),
                                    ),
                                    if (widget.visibleOnCurrentFrame) ...[
                                      const SizedBox(width: 4),
                                      Icon(
                                        Icons.circle,
                                        size: 5,
                                        color: colorScheme.primary,
                                      ),
                                    ],
                                    const SizedBox(width: 6),
                                    Expanded(
                                      child: Text(
                                        widget.trackLabel,
                                        textAlign: TextAlign.right,
                                        overflow: TextOverflow.ellipsis,
                                        style: Theme.of(context)
                                            .textTheme
                                            .labelSmall
                                            ?.copyWith(
                                              color:
                                                  colorScheme.onSurfaceVariant,
                                              fontSize: 10,
                                            ),
                                      ),
                                    ),
                                  ],
                                ),
                                Text(
                                  widget.subtitle,
                                  overflow: TextOverflow.ellipsis,
                                  maxLines: 1,
                                  style: Theme.of(context).textTheme.labelSmall
                                      ?.copyWith(
                                        color: colorScheme.onSurfaceVariant
                                            .withValues(alpha: 0.82),
                                        fontSize: 10,
                                      ),
                                ),
                                Text(
                                  widget.title,
                                  overflow: TextOverflow.ellipsis,
                                  maxLines: 1,
                                  style: Theme.of(context).textTheme.labelSmall
                                      ?.copyWith(
                                        color: widget.titleMuted
                                            ? colorScheme.onSurfaceVariant
                                                  .withValues(alpha: 0.70)
                                            : colorScheme.onSurface,
                                      ),
                                ),
                              ],
                            ),
                          ),
                        ],
                      ),
                    ),
                  ),
                  Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      _SelectionCheckbox(
                        buttonKey: ValueKey(
                          'quick-mark-sidebar-checkbox-${widget.mark.id}',
                        ),
                        checked: widget.checked,
                        onPressed: () =>
                            widget.onCheckedChanged(!widget.checked),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _SelectionCheckbox extends StatelessWidget {
  final Key buttonKey;
  final bool checked;
  final VoidCallback onPressed;

  const _SelectionCheckbox({
    required this.buttonKey,
    required this.checked,
    required this.onPressed,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Tooltip(
      message: checked
          ? AppLocalizations.of(context)!.quickMarkSidebarCancelSelection
          : AppLocalizations.of(context)!.quickMarkSidebarSelectMark,
      child: IconButton(
        key: buttonKey,
        onPressed: onPressed,
        icon: Icon(
          checked ? Icons.check_box : Icons.check_box_outline_blank,
          size: 18,
        ),
        color: checked ? colorScheme.primary : colorScheme.onSurfaceVariant,
        style: _quickMarkToggleButtonStyle(
          checked,
          colorScheme,
          const Size.square(26),
        ),
      ),
    );
  }
}

class _QuickMarkPreview extends StatelessWidget {
  final QuickMark mark;
  final QuickMarkThumbnail? thumbnail;

  const _QuickMarkPreview({required this.mark, this.thumbnail});

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    final thumbnail = this.thumbnail;
    final assetPath = thumbnail?.assetPath;
    return SizedBox(
      width: 52,
      height: 52,
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: colorScheme.surfaceContainerHighest,
          borderRadius: BorderRadius.circular(4),
        ),
        child: ClipRRect(
          borderRadius: BorderRadius.circular(4),
          child: Stack(
            fit: StackFit.expand,
            children: [
              if (thumbnail?.hasAsset == true && assetPath != null)
                Image.file(
                  File(assetPath),
                  fit: BoxFit.cover,
                  errorBuilder: (_, _, _) =>
                      CustomPaint(painter: _QuickMarkPreviewPainter(mark)),
                )
              else
                CustomPaint(painter: _QuickMarkPreviewPainter(mark)),
              if (thumbnail?.status == QuickMarkThumbnailStatus.queued)
                Positioned(
                  right: 4,
                  bottom: 4,
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      color: colorScheme.primary.withValues(alpha: 0.70),
                      shape: BoxShape.circle,
                    ),
                    child: const SizedBox.square(dimension: 6),
                  ),
                ),
              if (thumbnail?.status == QuickMarkThumbnailStatus.failed)
                Positioned(
                  right: 3,
                  bottom: 3,
                  child: Icon(
                    Icons.error_outline,
                    size: 10,
                    color: colorScheme.error.withValues(alpha: 0.80),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class _QuickMarkPreviewPainter extends CustomPainter {
  final QuickMark mark;

  const _QuickMarkPreviewPainter(this.mark);

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = mark.color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2.0
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;
    final rect = Rect.fromLTWH(13, 14, size.width - 26, size.height - 28);
    switch (mark.shape) {
      case QuickMarkShape.rectangle:
        canvas.drawRect(rect, paint);
      case QuickMarkShape.arrow:
        final start = Offset(rect.left, rect.top);
        final end = Offset(rect.right, rect.bottom);
        canvas.drawLine(start, end, paint);
        _drawArrowHead(canvas, paint, start, end);
    }
  }

  void _drawArrowHead(Canvas canvas, Paint paint, Offset start, Offset end) {
    final vector = end - start;
    if (vector.distance <= 0.01) return;
    final angle = math.atan2(vector.dy, vector.dx);
    const length = 9.0;
    const spread = math.pi / 7;
    for (final sign in [-1, 1]) {
      final point = end.translate(
        -math.cos(angle + spread * sign) * length,
        -math.sin(angle + spread * sign) * length,
      );
      canvas.drawLine(end, point, paint);
    }
  }

  @override
  bool shouldRepaint(covariant _QuickMarkPreviewPainter oldDelegate) {
    return oldDelegate.mark != mark;
  }
}

class _QuickMarkInspector extends StatefulWidget {
  final QuickMark mark;
  final String trackLabel;
  final List<Color> colors;
  final List<double> fontSizes;
  final List<double> strokeWidths;
  final ValueChanged<QuickMark> onChanged;
  final VoidCallback onJump;
  final VoidCallback onFocus;
  final VoidCallback onDelete;
  final String Function(Color color) colorLabel;

  const _QuickMarkInspector({
    required this.mark,
    required this.trackLabel,
    required this.colors,
    required this.fontSizes,
    required this.strokeWidths,
    required this.onChanged,
    required this.onJump,
    required this.onFocus,
    required this.onDelete,
    required this.colorLabel,
  });

  @override
  State<_QuickMarkInspector> createState() => _QuickMarkInspectorState();
}

class _QuickMarkInspectorState extends State<_QuickMarkInspector> {
  late final TextEditingController _textController;
  late final FocusNode _textFocusNode;

  @override
  void initState() {
    super.initState();
    _textController = TextEditingController(text: widget.mark.text);
    _textFocusNode = FocusNode(debugLabel: 'QuickMarkSidebarText');
  }

  @override
  void didUpdateWidget(covariant _QuickMarkInspector oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.mark.id != widget.mark.id) {
      _textController.text = widget.mark.text;
    }
  }

  @override
  void dispose() {
    _textController.dispose();
    _textFocusNode.dispose();
    super.dispose();
  }

  KeyEventResult _handleTextKeyEvent(FocusNode node, KeyEvent event) {
    if (event is! KeyDownEvent) return KeyEventResult.ignored;
    final key = event.logicalKey;
    if (key != LogicalKeyboardKey.enter &&
        key != LogicalKeyboardKey.numpadEnter) {
      return KeyEventResult.ignored;
    }
    final composing = _textController.value.composing;
    if (composing.isValid && !composing.isCollapsed) {
      return KeyEventResult.ignored;
    }
    final keyboard = HardwareKeyboard.instance;
    final wantsNewline =
        keyboard.isShiftPressed ||
        keyboard.isControlPressed ||
        keyboard.isAltPressed;
    if (wantsNewline) return KeyEventResult.ignored;
    node.unfocus();
    return KeyEventResult.handled;
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    return DecoratedBox(
      decoration: BoxDecoration(
        color: colorScheme.surfaceContainerLow,
        border: Border(top: BorderSide(color: colorScheme.outlineVariant)),
      ),
      child: Padding(
        padding: const EdgeInsets.all(4),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            SizedBox(
              height: 26,
              child: Row(
                children: [
                  Text(
                    l.quickMarkSidebarInspector,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: Theme.of(context).textTheme.labelMedium?.copyWith(
                      fontWeight: FontWeight.w700,
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      widget.trackLabel,
                      overflow: TextOverflow.ellipsis,
                      style: Theme.of(context).textTheme.labelSmall?.copyWith(
                        color: colorScheme.onSurfaceVariant,
                      ),
                    ),
                  ),
                ],
              ),
            ),
            Focus(
              onKeyEvent: _handleTextKeyEvent,
              child: TextField(
                controller: _textController,
                focusNode: _textFocusNode,
                keyboardType: TextInputType.multiline,
                textInputAction: TextInputAction.newline,
                maxLines: null,
                style: Theme.of(context).textTheme.bodySmall,
                onChanged: (text) =>
                    widget.onChanged(widget.mark.copyWith(text: text)),
                decoration: InputDecoration(
                  labelText: l.quickMarkSidebarAnnotationLabel,
                  floatingLabelBehavior: FloatingLabelBehavior.auto,
                  labelStyle: Theme.of(context).textTheme.bodySmall?.copyWith(
                    color: colorScheme.onSurfaceVariant,
                  ),
                  floatingLabelStyle: Theme.of(
                    context,
                  ).textTheme.labelSmall?.copyWith(color: colorScheme.primary),
                  isDense: true,
                  contentPadding: const EdgeInsets.fromLTRB(8, 8, 8, 8),
                  border: OutlineInputBorder(
                    borderRadius: BorderRadius.circular(5),
                  ),
                ),
              ),
            ),
            const SizedBox(height: 4),
            Wrap(
              spacing: 0,
              runSpacing: 4,
              crossAxisAlignment: WrapCrossAlignment.center,
              children: [
                _ToolToggleButton(
                  selected: widget.mark.syncAcrossTracks,
                  icon: Icons.sync,
                  tooltip: l.quickMarkSync,
                  onPressed: () => widget.onChanged(
                    widget.mark.copyWith(
                      syncAcrossTracks: !widget.mark.syncAcrossTracks,
                    ),
                  ),
                ),
                _ToolButton(
                  icon: Icons.center_focus_strong,
                  tooltip: l.quickMarkFocus,
                  onPressed: widget.onFocus,
                ),
                const _PanelGap(),
                const _PanelSeparator(),
                const _PanelGap(),
                _ToolButton(
                  icon: Icons.title,
                  tooltip: l.quickMarkText,
                  onPressed: _textFocusNode.requestFocus,
                ),
                _ToolToggleButton(
                  selected: widget.mark.textBold,
                  icon: Icons.format_bold,
                  tooltip: l.quickMarkBold,
                  onPressed: () => widget.onChanged(
                    widget.mark.copyWith(textBold: !widget.mark.textBold),
                  ),
                ),
                _FontSizeCombo(
                  value: widget.mark.textFontSize,
                  sizes: widget.fontSizes,
                  onChanged: (size) => widget.onChanged(
                    widget.mark.copyWith(textFontSize: size),
                  ),
                ),
                const _PanelGap(),
                const _PanelSeparator(),
                const _PanelGap(),
                _ToolToggleButton(
                  selected: widget.mark.shape == QuickMarkShape.rectangle,
                  icon: Icons.crop_square,
                  tooltip: l.quickMarkRectangle,
                  onPressed: () => widget.onChanged(
                    widget.mark.copyWith(shape: QuickMarkShape.rectangle),
                  ),
                ),
                _ToolToggleButton(
                  selected: widget.mark.shape == QuickMarkShape.arrow,
                  icon: Icons.arrow_forward,
                  tooltip: l.quickMarkArrow,
                  onPressed: () => widget.onChanged(
                    widget.mark.copyWith(shape: QuickMarkShape.arrow),
                  ),
                ),
                const _PanelGap(),
                const _PanelSeparator(),
                const _PanelGap(),
                _ColorCombo(
                  value: widget.mark.color,
                  colors: widget.colors,
                  labelFor: widget.colorLabel,
                  onChanged: (color) =>
                      widget.onChanged(widget.mark.copyWith(color: color)),
                ),
                const _PanelGap(),
                _StrokeCombo(
                  value: widget.mark.strokeWidth,
                  widths: widget.strokeWidths,
                  onChanged: (width) => widget.onChanged(
                    widget.mark.copyWith(strokeWidth: width),
                  ),
                ),
                const _PanelGap(),
                const _PanelSeparator(),
                const _PanelGap(),
                _DeleteToolButton(onPressed: widget.onDelete),
                const _PanelGap(),
                const _PanelSeparator(),
                const SizedBox(width: 6),
                _JumpPillButton(onPressed: widget.onJump),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _ToolToggleButton extends StatelessWidget {
  final bool selected;
  final IconData icon;
  final String tooltip;
  final VoidCallback onPressed;

  const _ToolToggleButton({
    required this.selected,
    required this.icon,
    required this.tooltip,
    required this.onPressed,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Tooltip(
      message: tooltip,
      child: IconButton(
        onPressed: onPressed,
        icon: Icon(icon, size: 18),
        color: selected ? colorScheme.primary : colorScheme.onSurfaceVariant,
        style: _quickMarkToggleButtonStyle(
          selected,
          colorScheme,
          const Size.square(_QuickMarkSidebarState._toolButtonSize),
        ),
      ),
    );
  }
}

class _ToolButton extends StatelessWidget {
  final IconData icon;
  final String tooltip;
  final VoidCallback onPressed;

  const _ToolButton({
    required this.icon,
    required this.tooltip,
    required this.onPressed,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Tooltip(
      message: tooltip,
      child: IconButton(
        onPressed: onPressed,
        icon: Icon(icon, size: 18),
        color: colorScheme.onSurfaceVariant,
        style: _quickMarkToggleButtonStyle(
          false,
          colorScheme,
          const Size.square(_QuickMarkSidebarState._toolButtonSize),
        ),
      ),
    );
  }
}

class _DeleteToolButton extends StatelessWidget {
  final VoidCallback? onPressed;

  const _DeleteToolButton({required this.onPressed});

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    return Tooltip(
      message: l.delete,
      child: IconButton(
        onPressed: onPressed,
        icon: const Icon(Icons.delete_outline, size: 18),
        style: _quickMarkDeleteButtonStyle(colorScheme),
      ),
    );
  }
}

class _PanelGap extends StatelessWidget {
  const _PanelGap();

  @override
  Widget build(BuildContext context) => const SizedBox(width: 4);
}

class _PanelSeparator extends StatelessWidget {
  const _PanelSeparator();

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 1,
      height: _QuickMarkSidebarState._toolButtonSize,
      child: Center(
        child: Container(
          width: 1,
          height: 18,
          color: Theme.of(context).colorScheme.outlineVariant,
        ),
      ),
    );
  }
}

class _JumpPillButton extends StatelessWidget {
  final VoidCallback onPressed;

  const _JumpPillButton({required this.onPressed});

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    return Tooltip(
      message: l.quickMarkSidebarJump,
      child: SizedBox(
        height: _QuickMarkSidebarState._toolButtonSize,
        child: TextButton.icon(
          onPressed: onPressed,
          icon: const Icon(Icons.near_me_outlined, size: 16),
          label: Text(l.quickMarkSidebarJump),
          style: TextButton.styleFrom(
            visualDensity: VisualDensity.compact,
            padding: const EdgeInsets.symmetric(horizontal: 10),
            minimumSize: const Size(0, _QuickMarkSidebarState._toolButtonSize),
            tapTargetSize: MaterialTapTargetSize.shrinkWrap,
            foregroundColor: colorScheme.primary,
            backgroundColor: colorScheme.primary.withValues(alpha: 0.12),
            shape: const StadiumBorder(),
            textStyle: Theme.of(
              context,
            ).textTheme.labelSmall?.copyWith(fontWeight: FontWeight.w700),
          ),
        ),
      ),
    );
  }
}

class _FontSizeCombo extends StatelessWidget {
  final double value;
  final List<double> sizes;
  final ValueChanged<double> onChanged;

  const _FontSizeCombo({
    required this.value,
    required this.sizes,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    final theme = Theme.of(context);
    return Tooltip(
      message: l.quickMarkTextSize,
      child: AppMenuCombo<double>(
        width: 48,
        height: _QuickMarkSidebarState._toolButtonSize,
        value: value,
        items: sizes,
        labelFor: (size) => size.round().toString(),
        onChanged: onChanged,
        textStyle: theme.textTheme.bodySmall?.copyWith(
          color: colorScheme.onSurfaceVariant,
          fontWeight: FontWeight.w600,
        ),
        menuTextStyle: theme.textTheme.bodySmall,
        maxMenuWidth: 92,
        itemHeight: 30,
        buttonPadding: const EdgeInsets.only(left: 6, right: 2),
        itemPadding: const EdgeInsets.only(left: 10, right: 14),
        borderRadius: BorderRadius.circular(4),
        backgroundColor: Colors.transparent,
        foregroundColor: colorScheme.onSurfaceVariant,
        iconSize: 16,
      ),
    );
  }
}

class _ColorCombo extends StatelessWidget {
  final Color value;
  final List<Color> colors;
  final String Function(Color color) labelFor;
  final ValueChanged<Color> onChanged;

  const _ColorCombo({
    required this.value,
    required this.colors,
    required this.labelFor,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    return Tooltip(
      message: AppLocalizations.of(context)!.quickMarkColor,
      child: AppMenuCombo<Color>(
        width: 46,
        height: _QuickMarkSidebarState._toolButtonSize,
        value: value,
        items: colors,
        labelFor: labelFor,
        onChanged: onChanged,
        minMenuWidth: 136,
        maxMenuWidth: 180,
        itemHeight: 30,
        showSelectedCheck: false,
        buttonPadding: const EdgeInsets.only(left: 6, right: 2),
        itemPadding: const EdgeInsets.only(left: 10, right: 12),
        borderRadius: BorderRadius.circular(4),
        backgroundColor: Colors.transparent,
        foregroundColor: colorScheme.onSurfaceVariant,
        iconSize: 16,
        buttonBuilder: (context, color, open) => Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            _ColorSwatch(color: color, selected: true),
            const SizedBox(width: 2),
            AppMenuComboArrow(
              open: open,
              size: 16,
              color: colorScheme.onSurfaceVariant,
            ),
          ],
        ),
        itemBuilder: (context, color, label, selected) => Row(
          children: [
            SizedBox(
              width: 22,
              child: selected
                  ? Icon(Icons.check, size: 16, color: colorScheme.primary)
                  : null,
            ),
            _ColorSwatch(color: color, selected: selected),
            const SizedBox(width: 10),
            Text(
              label,
              style: Theme.of(context).textTheme.bodySmall?.copyWith(
                color: selected ? colorScheme.primary : colorScheme.onSurface,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _ColorSwatch extends StatelessWidget {
  final Color color;
  final bool selected;

  const _ColorSwatch({required this.color, required this.selected});

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    final isWhite = color.computeLuminance() > 0.9;
    return DecoratedBox(
      decoration: BoxDecoration(
        color: color,
        shape: BoxShape.circle,
        border: Border.all(
          color: selected
              ? colorScheme.primary
              : isWhite
              ? colorScheme.outline
              : colorScheme.outlineVariant,
          width: selected ? 2 : 1,
        ),
      ),
      child: const SizedBox(width: 14, height: 14),
    );
  }
}

class _StrokeCombo extends StatelessWidget {
  final double value;
  final List<double> widths;
  final ValueChanged<double> onChanged;

  const _StrokeCombo({
    required this.value,
    required this.widths,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final colorScheme = Theme.of(context).colorScheme;
    final strokeLabel = l.quickMarkStrokeWidth(value.round());
    return Tooltip(
      message: l.quickMarkStroke,
      child: AppMenuCombo<double>(
        width: 80,
        height: _QuickMarkSidebarState._toolButtonSize,
        value: value,
        items: widths,
        labelFor: (width) => l.quickMarkStrokeWidth(width.round()),
        onChanged: onChanged,
        minMenuWidth: 132,
        maxMenuWidth: 160,
        itemHeight: 30,
        showSelectedCheck: false,
        buttonPadding: const EdgeInsets.only(left: 6, right: 2),
        itemPadding: const EdgeInsets.only(left: 10, right: 12),
        borderRadius: BorderRadius.circular(4),
        backgroundColor: Colors.transparent,
        foregroundColor: colorScheme.onSurfaceVariant,
        iconSize: 16,
        buttonBuilder: (context, width, open) => Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            SizedBox(
              width: 28,
              child: Text(
                strokeLabel,
                maxLines: 1,
                overflow: TextOverflow.clip,
                style: Theme.of(context).textTheme.labelSmall?.copyWith(
                  color: colorScheme.onSurfaceVariant,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ),
            const SizedBox(width: 3),
            _StrokePreview(width: width, previewWidth: 18),
            AppMenuComboArrow(
              open: open,
              size: 14,
              color: colorScheme.onSurfaceVariant,
            ),
          ],
        ),
        itemBuilder: (context, width, label, selected) => Row(
          children: [
            SizedBox(
              width: 22,
              child: selected
                  ? Icon(Icons.check, size: 16, color: colorScheme.primary)
                  : null,
            ),
            SizedBox(
              width: 34,
              child: Text(
                label,
                style: Theme.of(context).textTheme.bodySmall?.copyWith(
                  color: selected ? colorScheme.primary : colorScheme.onSurface,
                  fontWeight: selected ? FontWeight.w600 : null,
                ),
              ),
            ),
            const SizedBox(width: 10),
            _StrokePreview(width: width, previewWidth: 20),
          ],
        ),
      ),
    );
  }
}

class _StrokePreview extends StatelessWidget {
  final double width;
  final double previewWidth;

  const _StrokePreview({required this.width, this.previewWidth = 20});

  @override
  Widget build(BuildContext context) {
    return Container(
      width: previewWidth,
      height: width.clamp(1.0, 8.0).toDouble(),
      decoration: BoxDecoration(
        color: Theme.of(context).colorScheme.onSurfaceVariant,
        borderRadius: BorderRadius.circular(1),
      ),
    );
  }
}

ButtonStyle _quickMarkToggleButtonStyle(
  bool selected,
  ColorScheme colorScheme,
  Size size,
) {
  return IconButton.styleFrom(
    backgroundColor: selected
        ? colorScheme.primary.withValues(alpha: 0.16)
        : Colors.transparent,
    padding: EdgeInsets.zero,
    fixedSize: size,
    minimumSize: size,
    maximumSize: size,
    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(4)),
    hoverColor: selected
        ? colorScheme.primary.withValues(alpha: 0.20)
        : colorScheme.onSurfaceVariant.withValues(alpha: 0.10),
    focusColor: selected
        ? colorScheme.primary.withValues(alpha: 0.22)
        : colorScheme.onSurfaceVariant.withValues(alpha: 0.12),
    highlightColor: selected
        ? colorScheme.primary.withValues(alpha: 0.24)
        : colorScheme.onSurfaceVariant.withValues(alpha: 0.14),
  );
}

ButtonStyle _quickMarkDeleteButtonStyle(ColorScheme colorScheme) {
  const size = Size.square(_QuickMarkSidebarState._toolButtonSize);
  final warningStates = {
    WidgetState.hovered,
    WidgetState.focused,
    WidgetState.pressed,
  };
  return ButtonStyle(
    padding: const WidgetStatePropertyAll(EdgeInsets.zero),
    fixedSize: const WidgetStatePropertyAll(size),
    minimumSize: const WidgetStatePropertyAll(size),
    maximumSize: const WidgetStatePropertyAll(size),
    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
    shape: WidgetStatePropertyAll(
      RoundedRectangleBorder(borderRadius: BorderRadius.circular(4)),
    ),
    foregroundColor: WidgetStateProperty.resolveWith((states) {
      if (states.contains(WidgetState.disabled)) {
        return colorScheme.onSurfaceVariant.withValues(alpha: 0.38);
      }
      if (states.any(warningStates.contains)) return colorScheme.error;
      return colorScheme.onSurfaceVariant;
    }),
    backgroundColor: WidgetStateProperty.resolveWith((states) {
      if (states.contains(WidgetState.disabled)) return Colors.transparent;
      if (states.any(warningStates.contains)) {
        return colorScheme.error.withValues(alpha: 0.12);
      }
      return Colors.transparent;
    }),
    overlayColor: const WidgetStatePropertyAll(Colors.transparent),
  );
}

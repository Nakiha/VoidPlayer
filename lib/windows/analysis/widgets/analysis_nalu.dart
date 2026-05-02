import 'package:flutter/material.dart';

import '../../../analysis/analysis_ffi.dart';
import '../../../analysis/nalu_types.dart';
import '../../../l10n/app_localizations.dart';
import 'analysis_frame_utils.dart';

// ===========================================================================
// NALU Browser — search bar + scrollable list
// ===========================================================================

class AnalysisNaluBrowserView extends StatefulWidget {
  final List<NaluInfo> nalus;
  final int naluIndexBase;
  final int totalNalus;
  final AnalysisCodec codec;
  final int? selectedIdx;
  final ValueChanged<int> onSelected;
  final void Function(int start, int count) onWindowRequested;
  final String filter;
  final ValueChanged<String> onFilterChanged;

  const AnalysisNaluBrowserView({
    super.key,
    required this.nalus,
    this.naluIndexBase = 0,
    int? totalNalus,
    required this.codec,
    required this.selectedIdx,
    required this.onSelected,
    required this.onWindowRequested,
    required this.filter,
    required this.onFilterChanged,
  }) : totalNalus = totalNalus ?? nalus.length;

  @override
  State<AnalysisNaluBrowserView> createState() =>
      _AnalysisNaluBrowserViewState();
}

class _AnalysisNaluBrowserViewState extends State<AnalysisNaluBrowserView> {
  static const _itemExtent = 28.0;
  final _scrollController = ScrollController();
  List<NaluInfo>? _cachedNalus;
  AnalysisCodec? _cachedCodec;
  String? _cachedFilter;
  List<String> _cachedTypeNamesLower = const [];
  List<int> _cachedVisibleIndices = const [];

  @override
  void initState() {
    super.initState();
    _scrollController.addListener(_requestVisibleWindow);
    _scheduleScrollSelectedIntoView();
  }

  @override
  void didUpdateWidget(covariant AnalysisNaluBrowserView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.selectedIdx != oldWidget.selectedIdx ||
        widget.filter != oldWidget.filter ||
        widget.nalus.length != oldWidget.nalus.length ||
        widget.naluIndexBase != oldWidget.naluIndexBase ||
        widget.totalNalus != oldWidget.totalNalus) {
      _scheduleScrollSelectedIntoView();
    }
  }

  @override
  void dispose() {
    _scrollController.removeListener(_requestVisibleWindow);
    _scrollController.dispose();
    super.dispose();
  }

  void _requestVisibleWindow() {
    if (!_scrollController.hasClients || widget.filter.isNotEmpty) return;
    final position = _scrollController.position;
    final first = (position.pixels / _itemExtent).floor();
    final count = (position.viewportDimension / _itemExtent).ceil() + 24;
    widget.onWindowRequested(first, count);
  }

  List<int> _visibleIndices() {
    final filter = widget.filter.toLowerCase();
    if (!identical(_cachedNalus, widget.nalus) ||
        _cachedCodec != widget.codec) {
      _cachedNalus = widget.nalus;
      _cachedCodec = widget.codec;
      _cachedFilter = null;
      _cachedTypeNamesLower = [
        for (final nalu in widget.nalus)
          bitstreamUnitTypeName(widget.codec, nalu.nalType).toLowerCase(),
      ];
    }
    if (_cachedFilter == filter) return _cachedVisibleIndices;

    _cachedFilter = filter;
    _cachedVisibleIndices = [
      for (var i = 0; i < widget.nalus.length; i++)
        if (filter.isEmpty ||
            _cachedTypeNamesLower[i].contains(filter) ||
            '#${i + widget.naluIndexBase}'.contains(filter) ||
            '${widget.nalus[i].nalType}'.contains(filter))
          i + widget.naluIndexBase,
    ];
    return _cachedVisibleIndices;
  }

  void _scheduleScrollSelectedIntoView() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || !_scrollController.hasClients) return;
      final selectedIdx = widget.selectedIdx;
      if (selectedIdx == null) return;
      final displayIndex = _visibleIndices().indexOf(selectedIdx);
      if (displayIndex < 0) return;

      final position = _scrollController.position;
      final itemTop = displayIndex * _itemExtent;
      final itemBottom = itemTop + _itemExtent;
      final viewportTop = position.pixels;
      final viewportBottom = viewportTop + position.viewportDimension;

      double? target;
      if (itemTop < viewportTop) {
        target = itemTop;
      } else if (itemBottom > viewportBottom) {
        target = itemBottom - position.viewportDimension;
      }
      if (target == null) return;

      _scrollController.jumpTo(
        target
            .clamp(position.minScrollExtent, position.maxScrollExtent)
            .toDouble(),
      );
    });
  }

  @override
  Widget build(BuildContext context) {
    if (widget.totalNalus == 0 || widget.nalus.isEmpty) {
      return Center(
        child: Text(AppLocalizations.of(context)!.analysisNoNaluData),
      );
    }
    final theme = Theme.of(context);
    final filter = widget.filter.toLowerCase();
    final visible = _visibleIndices();
    final itemCount = filter.isEmpty ? widget.totalNalus : visible.length;

    return Column(
      children: [
        // Search bar — same height as list items (28px)
        Padding(
          padding: const EdgeInsets.all(4),
          child: TextField(
            onChanged: widget.onFilterChanged,
            style: theme.textTheme.bodySmall,
            decoration: InputDecoration(
              hintText: AppLocalizations.of(context)!.analysisFilterHint,
              hintStyle: theme.textTheme.bodySmall?.copyWith(
                color: theme.colorScheme.onSurfaceVariant.withValues(
                  alpha: 0.5,
                ),
              ),
              prefixIcon: Icon(
                Icons.search,
                size: 14,
                color: theme.colorScheme.onSurfaceVariant,
              ),
              prefixIconConstraints: const BoxConstraints(
                minWidth: 20,
                minHeight: 0,
              ),
              isDense: true,
              contentPadding: const EdgeInsets.symmetric(
                horizontal: 4,
                vertical: 6,
              ),
              border: OutlineInputBorder(
                borderRadius: BorderRadius.circular(4),
                borderSide: BorderSide(color: theme.colorScheme.outlineVariant),
              ),
              enabledBorder: OutlineInputBorder(
                borderRadius: BorderRadius.circular(4),
                borderSide: BorderSide(color: theme.colorScheme.outlineVariant),
              ),
            ),
          ),
        ),
        // Result count
        if (filter.isNotEmpty)
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 4),
            child: Align(
              alignment: Alignment.centerLeft,
              child: Text(
                '${visible.length} / ${widget.totalNalus}',
                style: theme.textTheme.labelSmall?.copyWith(
                  color: theme.colorScheme.onSurfaceVariant,
                ),
              ),
            ),
          ),
        // List
        Expanded(
          child: ExcludeSemantics(
            child: ListView.builder(
              controller: _scrollController,
              itemCount: itemCount,
              itemExtent: _itemExtent,
              itemBuilder: (context, displayIndex) {
                final origIdx = filter.isEmpty
                    ? displayIndex
                    : visible[displayIndex];
                final localIdx = origIdx - widget.naluIndexBase;
                if (localIdx < 0 || localIdx >= widget.nalus.length) {
                  WidgetsBinding.instance.addPostFrameCallback((_) {
                    if (mounted) widget.onWindowRequested(origIdx, 1);
                  });
                  return const SizedBox.shrink();
                }
                final n = widget.nalus[localIdx];
                final selected = origIdx == widget.selectedIdx;
                return InkWell(
                  onTap: () => widget.onSelected(origIdx),
                  child: Container(
                    color: selected
                        ? theme.colorScheme.primary.withValues(alpha: 0.15)
                        : null,
                    child: Row(
                      children: [
                        // Decorative color bar
                        Container(
                          width: 4,
                          height: 28,
                          color: bitstreamUnitDecorColor(
                            widget.codec,
                            n.nalType,
                            flags: n.flags,
                          ),
                        ),
                        const SizedBox(width: 8),
                        // Index
                        SizedBox(
                          width: 40,
                          child: Text(
                            '#$origIdx',
                            style: theme.textTheme.bodySmall?.copyWith(
                              color: theme.colorScheme.onSurfaceVariant,
                              fontFeatures: [
                                const FontFeature.tabularFigures(),
                              ],
                            ),
                          ),
                        ),
                        const SizedBox(width: 6),
                        // Type number
                        SizedBox(
                          width: 24,
                          child: Text(
                            '${n.nalType}',
                            style: theme.textTheme.bodySmall?.copyWith(
                              color: theme.colorScheme.onSurfaceVariant,
                              fontFeatures: [
                                const FontFeature.tabularFigures(),
                              ],
                            ),
                          ),
                        ),
                        const SizedBox(width: 6),
                        // Type name
                        Expanded(
                          child: Text(
                            bitstreamUnitTypeName(widget.codec, n.nalType),
                            style: theme.textTheme.bodySmall,
                          ),
                        ),
                      ],
                    ),
                  ),
                );
              },
            ),
          ),
        ),
      ],
    );
  }
}

// ===========================================================================
// NALU Detail
// ===========================================================================

class AnalysisNaluDetailView extends StatelessWidget {
  final NaluInfo? nalu;
  final int? frameIdx;
  final int frameIndexBase;
  final List<FrameInfo> frames;
  final AnalysisCodec codec;
  final AppLocalizations l;

  const AnalysisNaluDetailView({
    super.key,
    required this.nalu,
    this.frameIdx,
    this.frameIndexBase = 0,
    required this.frames,
    required this.codec,
    required this.l,
  });

  @override
  Widget build(BuildContext context) {
    if (nalu == null) {
      return Center(child: Text(l.analysisSelectNalu));
    }
    final n = nalu!;
    final theme = Theme.of(context);
    final ts = theme.textTheme.bodySmall!;
    final labelColor = theme.colorScheme.onSurfaceVariant;

    // NALU-level info
    final items = <_DetailRow>[
      _DetailRow(
        l.analysisType,
        '${bitstreamUnitTypeName(codec, n.nalType)} (${n.nalType})',
      ),
      _DetailRow(l.analysisTemporalId, '${n.temporalId}'),
      _DetailRow(l.analysisLayerId, '${n.layerId}'),
      _DetailRow(l.analysisOffset, '${n.offset}'),
      _DetailRow(l.analysisSize, l.analysisBytes(n.size)),
      _DetailRow('VCL', '${(n.flags & 0x01) != 0}'),
      _DetailRow('Slice', '${(n.flags & 0x02) != 0}'),
      _DetailRow('Keyframe', '${(n.flags & 0x04) != 0}'),
    ];

    // Frame-level info from VBS3 (when this NALU corresponds to a frame)
    final frameItems = <_DetailRow>[];
    if (frameIdx != null &&
        frameIdx! >= frameIndexBase &&
        frameIdx! < frameIndexBase + frames.length) {
      final f = frames[frameIdx! - frameIndexBase];
      final sliceName = analysisFrameSliceName(f);
      final nalName = bitstreamUnitTypeName(codec, f.nalType);

      frameItems.addAll([
        _DetailRow('Slice', '$sliceName (${f.sliceType})'),
        _DetailRow('NAL Unit', '$nalName (${f.nalType})'),
        _DetailRow('POC', '${f.poc}'),
        _DetailRow('Avg QP', '${f.avgQp}'),
        _DetailRow('Temporal ID', '${f.temporalId}'),
        _DetailRow(
          'Ref L0',
          f.numRefL0 > 0 ? f.refPocsL0.take(f.numRefL0).join(', ') : '-',
        ),
        _DetailRow(
          'Ref L1',
          f.numRefL1 > 0 ? f.refPocsL1.take(f.numRefL1).join(', ') : '-',
        ),
        _DetailRow('Pkt Size', l.analysisBytes(f.packetSize)),
        _DetailRow('PTS', '${f.pts}'),
        _DetailRow('DTS', '${f.dts}'),
      ]);
    }

    Widget section(String title, List<_DetailRow> rows) {
      if (rows.isEmpty) return const SizedBox.shrink();
      return Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(title, style: theme.textTheme.titleSmall),
          const SizedBox(height: 4),
          ...rows.map(
            (r) => Padding(
              padding: const EdgeInsets.symmetric(vertical: 1.5),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  SizedBox(
                    width: 80,
                    child: Text(r.label, style: ts.copyWith(color: labelColor)),
                  ),
                  Expanded(child: Text(r.value, style: ts)),
                ],
              ),
            ),
          ),
        ],
      );
    }

    return Padding(
      padding: const EdgeInsets.all(12),
      child: SingleChildScrollView(
        child: Align(
          alignment: Alignment.topLeft,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              section(l.analysisNaluDetail, items),
              if (frameItems.isNotEmpty) ...[
                const SizedBox(height: 12),
                const Divider(height: 1),
                const SizedBox(height: 8),
                section(l.analysisFrameInfo, frameItems),
              ],
            ],
          ),
        ),
      ),
    );
  }
}

class _DetailRow {
  final String label;
  final String value;
  const _DetailRow(this.label, this.value);
}

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
  final AnalysisCodec codec;
  final int? selectedIdx;
  final ValueChanged<int> onSelected;
  final String filter;
  final ValueChanged<String> onFilterChanged;

  const AnalysisNaluBrowserView({
    super.key,
    required this.nalus,
    required this.codec,
    required this.selectedIdx,
    required this.onSelected,
    required this.filter,
    required this.onFilterChanged,
  });

  @override
  State<AnalysisNaluBrowserView> createState() =>
      _AnalysisNaluBrowserViewState();
}

class _AnalysisNaluBrowserViewState extends State<AnalysisNaluBrowserView> {
  static const _itemExtent = 28.0;
  final _scrollController = ScrollController();

  @override
  void initState() {
    super.initState();
    _scheduleScrollSelectedIntoView();
  }

  @override
  void didUpdateWidget(covariant AnalysisNaluBrowserView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.selectedIdx != oldWidget.selectedIdx ||
        widget.filter != oldWidget.filter ||
        widget.nalus.length != oldWidget.nalus.length) {
      _scheduleScrollSelectedIntoView();
    }
  }

  @override
  void dispose() {
    _scrollController.dispose();
    super.dispose();
  }

  List<int> _visibleIndices() {
    final filter = widget.filter.toLowerCase();
    return [
      for (var i = 0; i < widget.nalus.length; i++)
        if (filter.isEmpty ||
            bitstreamUnitTypeName(
              widget.codec,
              widget.nalus[i].nalType,
            ).toLowerCase().contains(filter) ||
            '#$i'.contains(filter) ||
            '${widget.nalus[i].nalType}'.contains(filter))
          i,
    ];
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
    if (widget.nalus.isEmpty) {
      return Center(
        child: Text(AppLocalizations.of(context)!.analysisNoNaluData),
      );
    }
    final theme = Theme.of(context);
    final filter = widget.filter.toLowerCase();
    final visible = _visibleIndices();

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
                '${visible.length} / ${widget.nalus.length}',
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
              itemCount: visible.length,
              itemExtent: _itemExtent,
              itemBuilder: (context, displayIndex) {
                final origIdx = visible[displayIndex];
                final n = widget.nalus[origIdx];
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
  final List<FrameInfo> frames;
  final AnalysisCodec codec;
  final AppLocalizations l;

  const AnalysisNaluDetailView({
    super.key,
    required this.nalu,
    this.frameIdx,
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

    // Frame-level info from VBS2 (when this NALU corresponds to a frame)
    final frameItems = <_DetailRow>[];
    if (frameIdx != null && frameIdx! >= 0 && frameIdx! < frames.length) {
      final f = frames[frameIdx!];
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

class ReferenceEdgeSpan {
  final int sourceIndex;
  final int targetIndex;
  final int minIndex;
  final int maxIndex;

  ReferenceEdgeSpan({required this.sourceIndex, required this.targetIndex})
    : minIndex = sourceIndex < targetIndex ? sourceIndex : targetIndex,
      maxIndex = sourceIndex > targetIndex ? sourceIndex : targetIndex;

  String get key => '$sourceIndex:$targetIndex';

  bool intersectsRange(int start, int end) {
    return minIndex <= end && maxIndex >= start;
  }
}

class ReferenceEdgeSpanIndex {
  final _IntervalNode? _root;

  const ReferenceEdgeSpanIndex._(this._root);

  factory ReferenceEdgeSpanIndex(List<ReferenceEdgeSpan> edges) {
    return ReferenceEdgeSpanIndex._(_IntervalNode.build(edges));
  }

  static const empty = ReferenceEdgeSpanIndex._(null);

  List<ReferenceEdgeSpan> query({
    required int start,
    required int end,
    int margin = 0,
  }) {
    final root = _root;
    if (root == null || end < start) return const [];
    final expandedStart = start - margin;
    final expandedEnd = end + margin;
    final result = <ReferenceEdgeSpan>[];
    root.query(expandedStart, expandedEnd, result);
    return result;
  }
}

class _IntervalNode {
  final int center;
  final List<ReferenceEdgeSpan> centerEdges;
  final _IntervalNode? left;
  final _IntervalNode? right;

  const _IntervalNode({
    required this.center,
    required this.centerEdges,
    required this.left,
    required this.right,
  });

  static _IntervalNode? build(List<ReferenceEdgeSpan> edges) {
    if (edges.isEmpty) return null;
    final centers = [
      for (final edge in edges) (edge.minIndex + edge.maxIndex) ~/ 2,
    ]..sort();
    final center = centers[centers.length ~/ 2];
    final left = <ReferenceEdgeSpan>[];
    final right = <ReferenceEdgeSpan>[];
    final centerEdges = <ReferenceEdgeSpan>[];

    for (final edge in edges) {
      if (edge.maxIndex < center) {
        left.add(edge);
      } else if (edge.minIndex > center) {
        right.add(edge);
      } else {
        centerEdges.add(edge);
      }
    }

    return _IntervalNode(
      center: center,
      centerEdges: List.unmodifiable(centerEdges),
      left: build(left),
      right: build(right),
    );
  }

  void query(int start, int end, List<ReferenceEdgeSpan> out) {
    if (end < center) {
      for (final edge in centerEdges) {
        if (edge.minIndex <= end) out.add(edge);
      }
      left?.query(start, end, out);
      return;
    }
    if (start > center) {
      for (final edge in centerEdges) {
        if (edge.maxIndex >= start) out.add(edge);
      }
      right?.query(start, end, out);
      return;
    }

    out.addAll(centerEdges);
    left?.query(start, end, out);
    right?.query(start, end, out);
  }
}

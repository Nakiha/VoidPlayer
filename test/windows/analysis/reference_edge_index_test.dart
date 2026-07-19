import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/ui/charts/reference_edge_index.dart';

void main() {
  test('edge span index returns edges crossing the queried range', () {
    final index = ReferenceEdgeSpanIndex([
      ReferenceEdgeSpan(sourceIndex: 2, targetIndex: 5),
      ReferenceEdgeSpan(sourceIndex: 10, targetIndex: 80),
      ReferenceEdgeSpan(sourceIndex: 90, targetIndex: 120),
    ]);

    final keys = index
        .query(start: 40, end: 45)
        .map((edge) => edge.key)
        .toSet();

    expect(keys, {'10:80'});
  });

  test('edge span index excludes disjoint edges', () {
    final index = ReferenceEdgeSpanIndex([
      ReferenceEdgeSpan(sourceIndex: 2, targetIndex: 5),
      ReferenceEdgeSpan(sourceIndex: 90, targetIndex: 120),
    ]);

    final keys = index
        .query(start: 40, end: 45)
        .map((edge) => edge.key)
        .toSet();

    expect(keys, isEmpty);
  });

  test('edge span index applies a small query margin at boundaries', () {
    final index = ReferenceEdgeSpanIndex([
      ReferenceEdgeSpan(sourceIndex: 10, targetIndex: 20),
      ReferenceEdgeSpan(sourceIndex: 30, targetIndex: 40),
    ]);

    final keys = index
        .query(start: 21, end: 29, margin: 1)
        .map((edge) => edge.key)
        .toSet();

    expect(keys, {'10:20', '30:40'});
  });
}

import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/platform/window_bounds_policy.dart';

void main() {
  test('restores finite windows that meet the minimum size', () {
    expect(
      isRestorableWindowRect(const Rect.fromLTWH(20, 30, 800, 600)),
      isTrue,
    );
  });

  test('rejects missing undersized and non-finite window rects', () {
    expect(isRestorableWindowRect(null), isFalse);
    expect(
      isRestorableWindowRect(const Rect.fromLTWH(20, 30, 519, 600)),
      isFalse,
    );
    expect(
      isRestorableWindowRect(const Rect.fromLTWH(20, 30, 800, 359)),
      isFalse,
    );
    expect(
      isRestorableWindowRect(Rect.fromLTWH(double.nan, 30, 800, 600)),
      isFalse,
    );
  });

  test('honors platform screen visibility predicate', () {
    final rect = const Rect.fromLTWH(20, 30, 800, 600);

    expect(isRestorableWindowRect(rect, isOnScreen: (_) => true), isTrue);
    expect(isRestorableWindowRect(rect, isOnScreen: (_) => false), isFalse);
  });
}

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_ffi.dart';

void main() {
  test('analysis ffi can be imported without native symbols', () {
    final available = AnalysisFfi.isAvailable;
    if (!available) {
      expect(AnalysisFfi.unavailableReason, isA<AnalysisFfiUnavailable>());
    }
  });
}

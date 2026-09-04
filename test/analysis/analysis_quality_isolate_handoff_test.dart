import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_quality_service.dart';

void main() {
  group('NativeAnalysisQualityService isolate handoff', () {
    test('ffi backend does not leak unsendable request captures', () async {
      // Regression: the FFI backend (default on macOS) runs analyzeSync via
      // Isolate.run. The closure must only capture sendable primitives;
      // capturing the whole AnalysisQualityRequest would drag the UI
      // callbacks (which capture widget State/Completer objects) into the
      // isolate message and fail with "object is unsendable".
      final service = NativeAnalysisQualityService();
      final completer = Completer<void>();
      final request = AnalysisQualityRequest(
        videoPath: '/nonexistent/voidplayer-quality-regression.mp4',
        cancellationToken: AnalysisQualityCancellationToken(),
        onProgress: (_) {
          // Mimic the UI callback capturing unsendable objects.
          completer.isCompleted;
        },
      );

      await expectLater(
        service.analyze(request),
        throwsA(
          isNot(
            isA<ArgumentError>().having(
              (error) => error.message,
              'message',
              contains('unsendable'),
            ),
          ),
        ),
      );
    });
  });
}

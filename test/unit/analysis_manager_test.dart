import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_manager.dart';

void main() {
  test('overlay chunks use stable aligned frame windows', () {
    expect(
      AnalysisManager.overlayChunkRangeForFrame(
        frameCount: 573,
        targetFrame: 0,
      ),
      (startFrame: 0, endFrame: 63),
    );
    expect(
      AnalysisManager.overlayChunkRangeForFrame(
        frameCount: 573,
        targetFrame: 63,
      ),
      (startFrame: 0, endFrame: 63),
    );
    expect(
      AnalysisManager.overlayChunkRangeForFrame(
        frameCount: 573,
        targetFrame: 64,
      ),
      (startFrame: 64, endFrame: 127),
    );
    expect(
      AnalysisManager.overlayChunkRangeForFrame(
        frameCount: 573,
        targetFrame: 95,
      ),
      (startFrame: 64, endFrame: 127),
    );
    expect(
      AnalysisManager.overlayChunkRangeForFrame(
        frameCount: 573,
        targetFrame: 572,
      ),
      (startFrame: 512, endFrame: 572),
    );
  });

  test('overlay chunk range clamps short videos', () {
    expect(
      AnalysisManager.overlayChunkRangeForFrame(
        frameCount: 12,
        targetFrame: 99,
      ),
      (startFrame: 0, endFrame: 11),
    );
  });

  test('overlay chunk ranges prefetch adjacent windows near edges', () {
    expect(
      AnalysisManager.overlayChunkRangesForFrame(
        frameCount: 573,
        targetFrame: 80,
      ),
      [(startFrame: 64, endFrame: 127)],
    );
    expect(
      AnalysisManager.overlayChunkRangesForFrame(
        frameCount: 573,
        targetFrame: 64,
      ),
      [(startFrame: 64, endFrame: 127), (startFrame: 0, endFrame: 63)],
    );
    expect(
      AnalysisManager.overlayChunkRangesForFrame(
        frameCount: 573,
        targetFrame: 118,
      ),
      [(startFrame: 64, endFrame: 127), (startFrame: 128, endFrame: 191)],
    );
    expect(
      AnalysisManager.overlayChunkRangesForFrame(
        frameCount: 573,
        targetFrame: 572,
      ),
      [(startFrame: 512, endFrame: 572)],
    );
  });
}

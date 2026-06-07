import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/native_player/native_player_protocol.dart';

void main() {
  test('quick mark anchor normalizes presented frame timing', () {
    final anchor = QuickMarkAnchor.fromPresentedFrame(
      fileId: 7,
      fallbackPtsUs: 9000,
      timing: const PresentedFrameTiming(
        ptsUs: 12000,
        dtsUs: PresentedFrameTiming.noTimestampUs,
        durationUs: 33333,
        analysisFrameIndex: 4,
        frameIdentityMode: 3,
        sourcePacketIndex: 8,
        sourcePacketSize: 1024,
        sourcePacketPos: 2048,
        sourcePacketPtsUs: 120,
        sourcePacketDtsUs: 100,
      ),
    );

    expect(anchor.fileId, 7);
    expect(anchor.ptsUs, 12000);
    expect(anchor.dtsUs, 12000);
    expect(anchor.durationUs, 33333);
    expect(anchor.analysisFrameIndex, 4);
    expect(anchor.sourcePacketIndex, 8);
    expect(anchor.sourcePacketSize, 1024);
    expect(anchor.sourcePacketPos, 2048);
    expect(anchor.sourcePacketPtsUs, 120);
    expect(anchor.sourcePacketDtsUs, 100);
  });

  test('quick mark anchor matches by strongest available identity', () {
    const markAnchor = QuickMarkAnchor(
      fileId: 7,
      ptsUs: 12000,
      dtsUs: 10000,
      sourcePacketIndex: 8,
      sourcePacketSize: 1024,
      sourcePacketPos: 2048,
    );

    expect(
      markAnchor.matchesPresentedFrame(
        const QuickMarkAnchor(
          fileId: 7,
          ptsUs: 13000,
          dtsUs: 11000,
          sourcePacketIndex: 8,
          sourcePacketSize: 1024,
          sourcePacketPos: 2048,
        ),
      ),
      isTrue,
    );
    expect(
      markAnchor.matchesPresentedFrame(
        const QuickMarkAnchor(fileId: 7, ptsUs: 12000, dtsUs: 11000),
      ),
      isFalse,
    );
    expect(
      markAnchor.matchesPresentedFrame(
        const QuickMarkAnchor(fileId: 8, ptsUs: 12000, dtsUs: 10000),
      ),
      isFalse,
    );
  });
}

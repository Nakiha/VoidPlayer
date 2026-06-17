import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/actions/automation_action.dart';
import 'package:void_player/actions/player_action.dart';
import 'package:void_player/actions/player_assert.dart';
import 'package:void_player/app_log.dart';
import 'package:void_player/automation/automation_script.dart';

void main() {
  setUpAll(() async {
    await initLogging(['--log-level=flutter=OFF']);
  });

  test('parses and sorts release ui automation instructions', () {
    final file = File(
      '${Directory.systemTemp.path}${Platform.pathSeparator}void_player_automation_script_test.csv',
    );
    addTearDown(() {
      if (file.existsSync()) file.deleteSync();
    });
    file.writeAsStringSync('''
@WINDOW,800,600
# comment
2.0,QUIT,0
0.5,SET_RENDER_SIZE,320,180
0.1,PLAY
0.2,ADD_NETWORK_MEDIA,http://127.0.0.1:8765/h264_9s_1920x1080.mp4
0.3,ADD_SSH_MEDIA,user@example.com:/videos/clip.mp4
0.7,ASSERT_PLAYING
0.75,CAPTURE_VIEWPORT_REGION,roi,1,2,30,40,50,build/roi.png
0.76,DRAG_VIEWPORT_SAMPLE_NATIVE_DIAGNOSTIC_BOOL,120,-60,nativeCompositorSourceCacheActive,true,18,8,2
0.77,DEBUG_NATIVE_TIMING
0.8,SET_DECODE_MODE,forceSoftware
0.85,SET_AUDIBLE_TRACK,-1
0.9,ASSERT_CAPTURE_SPLIT_DIFF,cap,1.5,2.5,12
1.0,ASSERT_CAPTURE_DIFF,soft,hard,1.5,2.5,12
1.1,ASSERT_NATIVE_AUDIO,true,48000,1,-1
1.2,ASSERT_NATIVE_DIAGNOSTIC_STRING,presentationAdapter,cvpixelbuffer-bgra-copy
1.3,ASSERT_NATIVE_DIAGNOSTIC_BOOL,metalTextureValid,true
1.35,ASSERT_NATIVE_DIAGNOSTIC_INT_AT_LEAST,nativeCompositorEDRVideoMaxRGBX1000,1001
1.4,ASSERT_NATIVE_DIAGNOSTIC_INT_RANGE,textureWidth,320,1920
1.45,ASSERT_TRACK_METADATA,0,QuickTime / MOV,VideoToolbox / h264
1.5,CLOSE_MAIN_WINDOW
''');

    final instructions = parseAutomationScript(file.path);

    expect(instructions, hasLength(20));
    expect(instructions.map((i) => i.time.inMilliseconds), [
      100,
      200,
      300,
      500,
      700,
      750,
      760,
      770,
      800,
      850,
      900,
      1000,
      1100,
      1200,
      1300,
      1350,
      1400,
      1450,
      1500,
      2000,
    ]);
    expect(
      instructions[0],
      isA<ScriptAction>().having((i) => i.action, 'action', isA<Play>()),
    );
    expect(
      instructions[1],
      isA<ScriptAction>().having(
        (i) => i.action,
        'action',
        isA<AddNetworkMedia>().having(
          (a) => a.url,
          'url',
          'http://127.0.0.1:8765/h264_9s_1920x1080.mp4',
        ),
      ),
    );
    expect(
      instructions[2],
      isA<ScriptAction>().having(
        (i) => i.action,
        'action',
        isA<AddSshMedia>().having(
          (a) => a.remotePath,
          'remotePath',
          'user@example.com:/videos/clip.mp4',
        ),
      ),
    );
    expect(
      instructions[3],
      isA<ScriptAutomationAction>().having(
        (i) => i.action,
        'action',
        isA<SetRenderSize>(),
      ),
    );
    expect(
      instructions[4],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertPlaying>(),
      ),
    );
    expect(
      instructions[5],
      isA<ScriptAutomationAction>().having(
        (i) => i.action,
        'action',
        isA<CaptureViewportRegionAction>()
            .having((a) => a.nameId, 'nameId', 'roi')
            .having((a) => a.x, 'x', 1)
            .having((a) => a.y, 'y', 2)
            .having((a) => a.width, 'width', 30)
            .having((a) => a.height, 'height', 40)
            .having((a) => a.maxSize, 'maxSize', 50)
            .having((a) => a.outputPath, 'outputPath', 'build/roi.png'),
      ),
    );
    expect(
      instructions[6],
      isA<ScriptAutomationAction>().having(
        (i) => i.action,
        'action',
        isA<DragViewportSampleNativeDiagnosticBool>()
            .having((a) => a.dx, 'dx', 120)
            .having((a) => a.dy, 'dy', -60)
            .having((a) => a.key, 'key', 'nativeCompositorSourceCacheActive')
            .having((a) => a.value, 'value', isTrue)
            .having((a) => a.steps, 'steps', 18)
            .having((a) => a.stepMs, 'stepMs', 8)
            .having((a) => a.minMatches, 'minMatches', 2),
      ),
    );
    expect(
      instructions[7],
      isA<ScriptAutomationAction>().having(
        (i) => i.action,
        'action',
        isA<DebugNativeTimingAction>(),
      ),
    );
    expect(
      instructions[8],
      isA<ScriptSetDecodeMode>().having(
        (i) => i.mode.storageValue,
        'mode',
        'forceSoftware',
      ),
    );
    expect(
      instructions[9],
      isA<ScriptSetAudibleTrack>().having((i) => i.fileId, 'fileId', isNull),
    );
    expect(
      instructions[10],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertCaptureSplitDiff>(),
      ),
    );
    expect(
      instructions[11],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertCaptureDiff>(),
      ),
    );
    expect(
      instructions[12],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertNativeAudio>()
            .having((a) => a.available, 'available', isTrue)
            .having((a) => a.sampleRate, 'sampleRate', 48000)
            .having((a) => a.channels, 'channels', 1)
            .having((a) => a.activeTrack, 'activeTrack', -1),
      ),
    );
    expect(
      instructions[13],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertNativeDiagnosticString>()
            .having((a) => a.key, 'key', 'presentationAdapter')
            .having((a) => a.value, 'value', 'cvpixelbuffer-bgra-copy'),
      ),
    );
    expect(
      instructions[14],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertNativeDiagnosticBool>()
            .having((a) => a.key, 'key', 'metalTextureValid')
            .having((a) => a.value, 'value', isTrue),
      ),
    );
    expect(
      instructions[15],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertNativeDiagnosticIntAtLeast>()
            .having((a) => a.key, 'key', 'nativeCompositorEDRVideoMaxRGBX1000')
            .having((a) => a.minValue, 'minValue', 1001),
      ),
    );
    expect(
      instructions[16],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertNativeDiagnosticIntRange>()
            .having((a) => a.key, 'key', 'textureWidth')
            .having((a) => a.minValue, 'minValue', 320)
            .having((a) => a.maxValue, 'maxValue', 1920),
      ),
    );
    expect(
      instructions[17],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertTrackMetadata>()
            .having((a) => a.slot, 'slot', 0)
            .having((a) => a.formatName, 'formatName', 'QuickTime / MOV')
            .having((a) => a.decoderName, 'decoderName', 'VideoToolbox / h264'),
      ),
    );
    expect(instructions[18], isA<ScriptCloseMainWindow>());
    expect(
      instructions[19],
      isA<ScriptQuit>().having((i) => i.exitCode, 'exitCode', 0),
    );
  });

  test('parses TOGGLE_MARKS_SIDEBAR', () {
    final file = File(
      '${Directory.systemTemp.path}${Platform.pathSeparator}void_player_marks_sidebar_script_test.csv',
    );
    addTearDown(() {
      if (file.existsSync()) file.deleteSync();
    });
    file.writeAsStringSync('0.1,TOGGLE_MARKS_SIDEBAR\n');

    final instructions = parseAutomationScript(file.path);

    expect(instructions, hasLength(1));
    expect(
      instructions.single,
      isA<ScriptAutomationAction>().having(
        (i) => i.action,
        'action',
        isA<ToggleMarksSidebar>(),
      ),
    );
  });

  test('parses SET_MEDIA_SOURCE_ID with slot and source id', () {
    final file = File(
      '${Directory.systemTemp.path}${Platform.pathSeparator}void_player_source_id_script_test.csv',
    );
    addTearDown(() {
      if (file.existsSync()) file.deleteSync();
    });
    file.writeAsStringSync('''
0.1,SET_MEDIA_SOURCE_ID,1,clip01_v2
0.2,SET_MEDIA_SOURCE_ID,0
0.3,SET_MEDIA_SOURCE_ID,0,   
''');

    final instructions = parseAutomationScript(file.path);

    expect(instructions, hasLength(1), reason: 'malformed lines are dropped');
    final instruction = instructions.single;
    expect(instruction, isA<ScriptAutomationAction>());
    final action =
        (instruction as ScriptAutomationAction).action as SetMediaSourceId;
    expect(action.slotIndex, 1);
    expect(action.sourceId, 'clip01_v2');
  });

  test('parses EXPORT_MARKS with output path', () {
    final file = File(
      '${Directory.systemTemp.path}${Platform.pathSeparator}void_player_export_marks_script_test.csv',
    );
    addTearDown(() {
      if (file.existsSync()) file.deleteSync();
    });
    file.writeAsStringSync('''
0.1,EXPORT_MARKS,build/verdicts.json
0.2,EXPORT_MARKS
''');

    final instructions = parseAutomationScript(file.path);

    expect(instructions, hasLength(1));
    final action =
        (instructions.single as ScriptAutomationAction).action as ExportMarks;
    expect(action.outputPath, 'build/verdicts.json');
  });

  test('parses ADD_QUICK_MARK with rect and judgment fields', () {
    final file = File(
      '${Directory.systemTemp.path}${Platform.pathSeparator}void_player_add_mark_script_test.csv',
    );
    addTearDown(() {
      if (file.existsSync()) file.deleteSync();
    });
    file.writeAsStringSync('''
0.1,ADD_QUICK_MARK,0,0.25,0.25,0.2,0.15,banding,3
0.2,ADD_QUICK_MARK,1,0.1,0.1,0.3,0.3
0.3,ADD_QUICK_MARK,0,0.1
0.4,ASSERT_MARK_COUNT,2
''');

    final instructions = parseAutomationScript(file.path);

    expect(instructions, hasLength(3));
    final tagged =
        (instructions[0] as ScriptAutomationAction).action as AddQuickMark;
    expect(tagged.slotIndex, 0);
    expect(tagged.left, 0.25);
    expect(tagged.height, 0.15);
    expect(tagged.defectType, 'banding');
    expect(tagged.severity, 3);

    final bare =
        (instructions[1] as ScriptAutomationAction).action as AddQuickMark;
    expect(bare.defectType, isNull);
    expect(bare.severity, isNull);

    final assertion = (instructions[2] as ScriptAssert).assertion;
    expect(assertion, isA<AssertMarkCount>());
    expect((assertion as AssertMarkCount).count, 2);
  });
}

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/actions/automation_action.dart';
import 'package:void_player/actions/player_action.dart';
import 'package:void_player/actions/player_assert.dart';
import 'package:void_player/automation/automation_script.dart';

void main() {
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
0.8,SET_DECODE_MODE,forceSoftware
0.85,SET_AUDIBLE_TRACK,-1
0.9,ASSERT_CAPTURE_SPLIT_DIFF,cap,1.5,2.5,12
1.0,ASSERT_CAPTURE_DIFF,soft,hard,1.5,2.5,12
1.1,ASSERT_NATIVE_AUDIO,true,48000,1,-1
1.2,ASSERT_NATIVE_DIAGNOSTIC_STRING,presentationAdapter,cvpixelbuffer-bgra-copy
1.3,CLOSE_MAIN_WINDOW
''');

    final instructions = parseAutomationScript(file.path);

    expect(instructions, hasLength(13));
    expect(instructions.map((i) => i.time.inMilliseconds), [
      100,
      200,
      300,
      500,
      700,
      800,
      850,
      900,
      1000,
      1100,
      1200,
      1300,
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
      isA<ScriptSetDecodeMode>().having(
        (i) => i.mode.storageValue,
        'mode',
        'forceSoftware',
      ),
    );
    expect(
      instructions[6],
      isA<ScriptSetAudibleTrack>().having((i) => i.fileId, 'fileId', isNull),
    );
    expect(
      instructions[7],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertCaptureSplitDiff>(),
      ),
    );
    expect(
      instructions[8],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertCaptureDiff>(),
      ),
    );
    expect(
      instructions[9],
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
      instructions[10],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertNativeDiagnosticString>()
            .having((a) => a.key, 'key', 'presentationAdapter')
            .having((a) => a.value, 'value', 'cvpixelbuffer-bgra-copy'),
      ),
    );
    expect(instructions[11], isA<ScriptCloseMainWindow>());
    expect(
      instructions[12],
      isA<ScriptQuit>().having((i) => i.exitCode, 'exitCode', 0),
    );
  });
}

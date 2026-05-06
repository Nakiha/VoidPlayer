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
0.7,ASSERT_PLAYING
0.8,SET_DECODE_MODE,forceSoftware
0.9,ASSERT_CAPTURE_SPLIT_DIFF,cap,1.5,2.5,12
1.0,ASSERT_CAPTURE_DIFF,soft,hard,1.5,2.5,12
''');

    final instructions = parseAutomationScript(file.path);

    expect(instructions, hasLength(7));
    expect(instructions.map((i) => i.time.inMilliseconds), [
      100,
      500,
      700,
      800,
      900,
      1000,
      2000,
    ]);
    expect(
      instructions[0],
      isA<ScriptAction>().having((i) => i.action, 'action', isA<Play>()),
    );
    expect(
      instructions[1],
      isA<ScriptAutomationAction>().having(
        (i) => i.action,
        'action',
        isA<SetRenderSize>(),
      ),
    );
    expect(
      instructions[2],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertPlaying>(),
      ),
    );
    expect(
      instructions[3],
      isA<ScriptSetDecodeMode>().having(
        (i) => i.mode.storageValue,
        'mode',
        'forceSoftware',
      ),
    );
    expect(
      instructions[4],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertCaptureSplitDiff>(),
      ),
    );
    expect(
      instructions[5],
      isA<ScriptAssert>().having(
        (i) => i.assertion,
        'assertion',
        isA<AssertCaptureDiff>(),
      ),
    );
    expect(
      instructions[6],
      isA<ScriptQuit>().having((i) => i.exitCode, 'exitCode', 0),
    );
  });
}

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/windows/process_log_args.dart';

void main() {
  test('redacts analysis ipc token passed with equals syntax', () {
    expect(
      redactProcessArgsForLog([
        '--standalone-analysis',
        '--analysis-ipc-token=secret-token',
        '--hash=abc',
      ]),
      [
        '--standalone-analysis',
        '--analysis-ipc-token=$redactedProcessArgValue',
        '--hash=abc',
      ],
    );
  });

  test('redacts analysis ipc token passed as the next argument', () {
    expect(
      redactProcessArgsForLog([
        '--analysis-ipc-token',
        'secret-token',
        '--silent-ui-test',
      ]),
      ['--analysis-ipc-token', redactedProcessArgValue, '--silent-ui-test'],
    );
  });
}

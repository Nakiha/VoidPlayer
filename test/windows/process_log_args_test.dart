import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/app_log.dart';

void main() {
  test('Flutter process logs always use the main-window role', () {
    expect(LogConfig.defaultsFor(const []).processRole, 'main');
    expect(
      LogConfig.defaultsFor(const [
        '--test-script',
        'smoke.csv',
        '--silent-ui-test',
      ]).processRole,
      'main',
    );
  });
}

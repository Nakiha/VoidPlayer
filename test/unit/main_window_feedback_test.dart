import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/native_player/native_player_protocol.dart';
import 'package:void_player/windows/main/main_window.dart';

void main() {
  test('user action failure message prefers PlatformException message', () {
    expect(
      formatMainWindowUserActionFailure(
        'adjust track offset',
        PlatformException(code: 'native-error', message: 'offset failed'),
      ),
      'adjust track offset failed: offset failed',
    );
  });

  test('user action failure message falls back to PlatformException code', () {
    expect(
      formatMainWindowUserActionFailure(
        'seek',
        PlatformException(code: 'native-error'),
      ),
      'seek failed: native-error',
    );
  });

  test('user action failure message summarizes native protocol errors', () {
    expect(
      formatMainWindowUserActionFailure(
        'refresh tracks',
        const NativeProtocolException(
          context: 'getTracks',
          reason: 'expected a list payload',
          payload: null,
        ),
      ),
      'refresh tracks failed: expected a list payload',
    );
  });
}

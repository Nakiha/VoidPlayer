import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/app_log.dart';
import 'package:void_player/utils/async_guard.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  setUpAll(() async {
    await initLogging(['--log-level=flutter=OFF']);
  });

  test('runGuardedAction reports synchronous failures', () async {
    Object? reportedError;

    await runGuardedAction(
      'sync action',
      () => throw StateError('sync failed'),
      onError: (error, _) {
        reportedError = error;
      },
    );

    expect(reportedError, isA<StateError>());
    expect(reportedError.toString(), contains('sync failed'));
  });

  test('runGuardedAction reports asynchronous failures', () async {
    Object? reportedError;

    await runGuardedAction(
      'async action',
      () async => throw StateError('async failed'),
      onError: (error, _) {
        reportedError = error;
      },
    );

    expect(reportedError, isA<StateError>());
    expect(reportedError.toString(), contains('async failed'));
  });

  test('fireGuardedAction reports fire-and-forget failures', () async {
    Object? reportedError;

    fireGuardedAction(
      'fire action',
      () async => throw StateError('fire failed'),
      onError: (error, _) {
        reportedError = error;
      },
    );
    await Future<void>.delayed(Duration.zero);
    await Future<void>.delayed(Duration.zero);

    expect(reportedError, isA<StateError>());
    expect(reportedError.toString(), contains('fire failed'));
  });
}

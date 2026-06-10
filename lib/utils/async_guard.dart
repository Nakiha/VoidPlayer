import 'dart:async';

import '../app_log.dart';

void fireAndLog(String operation, Future<void> future) {
  unawaited(
    future.catchError((Object error, StackTrace stack) {
      log.severe('$operation failed', error, stack);
    }),
  );
}

Future<void> runGuardedAction(
  String operation,
  FutureOr<void> Function() action, {
  void Function(Object error, StackTrace stack)? onError,
}) async {
  try {
    final result = action();
    if (result is Future) {
      await result;
    }
  } catch (error, stack) {
    log.severe('$operation failed', error, stack);
    onError?.call(error, stack);
  }
}

void fireGuardedAction(
  String operation,
  FutureOr<void> Function() action, {
  void Function(Object error, StackTrace stack)? onError,
}) {
  unawaited(runGuardedAction(operation, action, onError: onError));
}

import 'dart:async';

import '../app_log.dart';

void fireAndLog(String operation, Future<void> future) {
  unawaited(
    future.catchError((Object error, StackTrace stack) {
      log.severe('$operation failed', error, stack);
    }),
  );
}

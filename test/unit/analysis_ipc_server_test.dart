import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/app_log.dart';
import 'package:void_player/windows/analysis/ipc/analysis_ipc_server.dart';

void main() {
  test('AnalysisIpcServer tracks active authorized clients', () async {
    await initLogging(['--log-level=flutter=OFF']);
    final server = AnalysisIpcServer();
    addTearDown(server.dispose);

    await server.start();
    expect(server.hasClients, isFalse);

    final socket = await Socket.connect(
      InternetAddress.loopbackIPv4,
      server.port!,
    );
    socket.writeln(jsonEncode({'type': 'hello', 'token': server.token}));
    await expectLater(
      Stream.periodic(
            const Duration(milliseconds: 10),
            (_) => server.hasClients,
          )
          .firstWhere((hasClients) => hasClients)
          .timeout(const Duration(seconds: 2)),
      completion(isTrue),
    );

    socket.destroy();
    await expectLater(
      Stream.periodic(
            const Duration(milliseconds: 10),
            (_) => server.hasClients,
          )
          .firstWhere((hasClients) => !hasClients)
          .timeout(const Duration(seconds: 2)),
      completion(isFalse),
    );
  });
}

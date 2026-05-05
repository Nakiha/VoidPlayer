import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/app_log.dart';
import 'package:void_player/windows/analysis/ipc/analysis_ipc_server.dart';

void main() {
  setUpAll(() async {
    await initLogging(['--log-level=flutter=OFF']);
  });

  test('AnalysisIpcServer tracks active authorized clients', () async {
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

  test(
    'AnalysisIpcServer drops clients that do not handshake in time',
    () async {
      final server = AnalysisIpcServer(
        handshakeTimeout: const Duration(milliseconds: 40),
      );
      addTearDown(server.dispose);

      await server.start();
      final socket = await Socket.connect(
        InternetAddress.loopbackIPv4,
        server.port!,
      );
      addTearDown(socket.destroy);

      await Future<void>.delayed(const Duration(milliseconds: 120));

      expect(server.hasClients, isFalse);
      socket.writeln(jsonEncode({'type': 'hello', 'token': server.token}));
      await expectLater(socket.first, throwsA(isA<StateError>()));
    },
  );

  test('AnalysisIpcServer drops overlong json lines', () async {
    final server = AnalysisIpcServer(maxLineLength: 16);
    addTearDown(server.dispose);

    await server.start();
    final socket = await Socket.connect(
      InternetAddress.loopbackIPv4,
      server.port!,
    );
    addTearDown(socket.destroy);

    socket.write('{"type":"hello","token":"${server.token}"}\n');
    await Future<void>.delayed(const Duration(milliseconds: 50));

    expect(server.hasClients, isFalse);
    await expectLater(socket.first, throwsA(isA<StateError>()));
  });
}

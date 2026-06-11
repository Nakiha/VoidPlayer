import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:void_player/agent/agent_protocol_server.dart';
import 'package:void_player/app_log.dart';

class _FakeHandler implements AgentRequestHandler {
  final calls = <(String, Map<String, Object?>)>[];

  @override
  Future<Map<String, Object?>> handleRequest(
    String method,
    Map<String, Object?> params,
  ) async {
    calls.add((method, params));
    switch (method) {
      case 'echo':
        return {'echoed': params['value']};
      case 'boom':
        throw const AgentRequestException('denied', 'not allowed');
      case 'crash':
        throw StateError('handler blew up');
      default:
        throw AgentRequestException('unknownMethod', 'unknown method $method');
    }
  }
}

class _Client {
  final Socket socket;
  final StreamIterator<String> _lines;

  _Client._(this.socket, this._lines);

  static Future<_Client> connect(int port) async {
    final socket = await Socket.connect(InternetAddress.loopbackIPv4, port);
    final lines = StreamIterator(
      socket
          .cast<List<int>>()
          .transform(utf8.decoder)
          .transform(const LineSplitter()),
    );
    return _Client._(socket, lines);
  }

  void send(Map<String, Object?> message) {
    socket.writeln(jsonEncode(message));
  }

  Future<Map<String, Object?>> read() async {
    final hasNext = await _lines.moveNext().timeout(const Duration(seconds: 5));
    if (!hasNext) {
      throw StateError('connection closed before a response arrived');
    }
    return jsonDecode(_lines.current) as Map<String, Object?>;
  }

  Future<bool> get closed => _lines
      .moveNext()
      .timeout(const Duration(seconds: 5))
      .then((hasNext) => !hasNext);

  void destroy() => socket.destroy();
}

void main() {
  setUpAll(() async {
    await initLogging(['--log-level=flutter=OFF']);
  });

  late Directory dir;
  late _FakeHandler handler;
  late AgentProtocolServer server;
  late String connectionFile;

  setUp(() async {
    dir = await Directory.systemTemp.createTemp('void_player_agent_test_');
    handler = _FakeHandler();
    server = AgentProtocolServer(handler: handler);
    connectionFile = p.join(dir.path, 'agent', 'connection.json');
    await server.start(connectionFilePath: connectionFile);
  });

  tearDown(() async {
    await server.dispose();
    if (await dir.exists()) await dir.delete(recursive: true);
  });

  test('writes connection file with port and token', () async {
    final payload =
        jsonDecode(await File(connectionFile).readAsString())
            as Map<String, Object?>;
    expect(payload['protocolVersion'], agentProtocolVersion);
    expect(payload['port'], server.port);
    expect(payload['token'], server.token);
    expect(payload['pid'], isA<int>());
  });

  test('rejects clients with a bad token', () async {
    final client = await _Client.connect(server.port!);
    client.send({'type': 'hello', 'token': 'wrong'});
    expect(await client.closed, isTrue);
  });

  test('acknowledges handshake and dispatches requests', () async {
    final client = await _Client.connect(server.port!);
    client.send({'type': 'hello', 'token': server.token});
    final ack = await client.read();
    expect(ack['type'], 'helloAck');
    expect(ack['protocolVersion'], agentProtocolVersion);

    client.send({
      'id': 7,
      'method': 'echo',
      'params': {'value': 'ping'},
    });
    final response = await client.read();
    expect(response['id'], 7);
    expect((response['result'] as Map)['echoed'], 'ping');
    expect(handler.calls.single.$1, 'echo');
    client.destroy();
  });

  test('maps handler exceptions to protocol errors', () async {
    final client = await _Client.connect(server.port!);
    client.send({'type': 'hello', 'token': server.token});
    await client.read();

    client.send({'id': 1, 'method': 'boom'});
    final denied = await client.read();
    expect((denied['error'] as Map)['code'], 'denied');

    client.send({'id': 2, 'method': 'crash'});
    final crashed = await client.read();
    expect((crashed['error'] as Map)['code'], 'internal');

    client.send({'id': 3, 'method': 'nope'});
    final unknown = await client.read();
    expect((unknown['error'] as Map)['code'], 'unknownMethod');
    client.destroy();
  });

  test('rejects requests without id or method', () async {
    final client = await _Client.connect(server.port!);
    client.send({'type': 'hello', 'token': server.token});
    await client.read();

    client.send({'method': 'echo'});
    final response = await client.read();
    expect((response['error'] as Map)['code'], 'badRequest');
    expect(handler.calls, isEmpty);
    client.destroy();
  });

  test('drops clients sending malformed json', () async {
    final client = await _Client.connect(server.port!);
    client.send({'type': 'hello', 'token': server.token});
    await client.read();

    client.socket.writeln('not json');
    expect(await client.closed, isTrue);
  });

  test('dispose removes the connection file', () async {
    await server.dispose();
    expect(await File(connectionFile).exists(), isFalse);
  });
}

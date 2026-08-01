import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import '../app_log.dart';
import '../utils/async_guard.dart';
import '../utils/bounded_line_splitter.dart';

/// Wire protocol version spoken by [AgentProtocolServer].
const int agentProtocolVersion = 1;

const Duration agentProtocolHandshakeTimeout = Duration(seconds: 5);
const int agentProtocolMaxLineLength = 4 * 1024 * 1024;

/// Raised by an [AgentRequestHandler] to return a structured protocol error.
class AgentRequestException implements Exception {
  final String code;
  final String message;

  const AgentRequestException(this.code, this.message);

  @override
  String toString() => 'AgentRequestException($code: $message)';
}

/// Application surface behind the protocol. The server owns transport,
/// authentication, and framing; the handler owns meaning.
abstract class AgentRequestHandler {
  Future<Map<String, Object?>> handleRequest(
    String method,
    Map<String, Object?> params,
  );
}

/// Resident loopback control channel for agents.
///
/// Transport uses line-delimited JSON over a loopback TCP socket with a secure
/// random token handshake. On start the port and
/// token are written atomically to a connection file the launching agent
/// polls. After `{"type":"hello","token":...}` is acknowledged, the client
/// sends `{"id","method","params"}` requests and receives `{"id","result"}`
/// or `{"id","error":{"code","message"}}` responses.
class AgentProtocolServer {
  final AgentRequestHandler handler;
  final Duration handshakeTimeout;
  final int maxLineLength;

  ServerSocket? _server;
  final _clients = <Socket>{};
  String? _token;
  String? _connectionFilePath;

  bool get isStarted => _server != null;
  int? get port => _server?.port;
  String? get token => _token;

  AgentProtocolServer({
    required this.handler,
    this.handshakeTimeout = agentProtocolHandshakeTimeout,
    this.maxLineLength = agentProtocolMaxLineLength,
  });

  Future<void> start({required String connectionFilePath}) async {
    if (_server != null) return;
    _token = _generateToken();
    _server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    _server!.listen(
      _handleClient,
      onError: (Object error, StackTrace stack) {
        log.warning('[AgentProtocolServer] accept failed: $error');
      },
    );
    _connectionFilePath = connectionFilePath;
    await _writeConnectionFile(connectionFilePath);
    log.info(
      '[AgentProtocolServer] listening on 127.0.0.1:${_server!.port}, '
      'connection file: $connectionFilePath',
    );
  }

  Future<void> dispose() async {
    final clients = List<Socket>.from(_clients);
    _clients.clear();
    for (final client in clients) {
      client.destroy();
    }
    await _server?.close();
    _server = null;
    _token = null;
    final connectionFilePath = _connectionFilePath;
    _connectionFilePath = null;
    if (connectionFilePath != null) {
      try {
        final file = File(connectionFilePath);
        if (await file.exists()) await file.delete();
      } catch (error, stack) {
        logFine(
          '[AgentProtocolServer] connection file cleanup failed',
          error,
          stack,
        );
      }
    }
  }

  Future<void> _writeConnectionFile(String path) async {
    final file = File(path);
    await file.parent.create(recursive: true);
    final payload = jsonEncode({
      'protocolVersion': agentProtocolVersion,
      'port': _server!.port,
      'token': _token,
      'pid': pid,
    });
    // Write-then-rename so a polling agent never reads a partial file.
    final tmp = File('$path.tmp');
    await tmp.writeAsString(payload, flush: true);
    await tmp.rename(path);
  }

  void _handleClient(Socket socket) {
    var authorized = false;
    StreamSubscription<String>? subscription;
    final handshakeTimer = Timer(handshakeTimeout, () {
      if (authorized) return;
      log.warning('[AgentProtocolServer] handshake timed out');
      socket.destroy();
    });

    void detach() {
      handshakeTimer.cancel();
      _clients.remove(socket);
      final activeSubscription = subscription;
      if (activeSubscription != null) {
        fireAndLogFine(
          'cancel agent protocol client subscription',
          activeSubscription.cancel(),
        );
      }
    }

    subscription = socket
        .cast<List<int>>()
        .transform(utf8.decoder)
        .transform(BoundedLineSplitter(maxLineLength: maxLineLength))
        .listen(
          (line) {
            Map<String, Object?> message;
            try {
              final decoded = jsonDecode(line);
              if (decoded is! Map<String, Object?>) {
                throw const FormatException('expected a JSON object');
              }
              message = decoded;
            } catch (e) {
              log.warning('[AgentProtocolServer] malformed message: $e');
              socket.destroy();
              return;
            }

            if (!authorized) {
              if (message['type'] != 'hello' || message['token'] != _token) {
                log.warning('[AgentProtocolServer] rejected client');
                socket.destroy();
                return;
              }
              authorized = true;
              handshakeTimer.cancel();
              _clients.add(socket);
              _send(socket, {
                'type': 'helloAck',
                'protocolVersion': agentProtocolVersion,
              });
              log.info('[AgentProtocolServer] agent connected');
              return;
            }

            fireAndLog(
              'dispatch agent protocol request',
              _dispatchRequest(socket, message),
            );
          },
          onDone: detach,
          onError: (Object error, StackTrace stack) {
            log.warning('[AgentProtocolServer] client error: $error');
            socket.destroy();
            detach();
          },
          cancelOnError: true,
        );
  }

  Future<void> _dispatchRequest(
    Socket socket,
    Map<String, Object?> message,
  ) async {
    final id = message['id'];
    final method = message['method'];
    if (id == null || method is! String || method.isEmpty) {
      _send(socket, {
        'id': id,
        'error': {
          'code': 'badRequest',
          'message': 'requests need an id and a method',
        },
      });
      return;
    }
    final rawParams = message['params'];
    final params = rawParams is Map
        ? Map<String, Object?>.fromEntries(
            rawParams.entries
                .where((entry) => entry.key is String)
                .map((entry) => MapEntry(entry.key as String, entry.value)),
          )
        : <String, Object?>{};
    try {
      final result = await handler.handleRequest(method, params);
      _send(socket, {'id': id, 'result': result});
    } on AgentRequestException catch (e) {
      _send(socket, {
        'id': id,
        'error': {'code': e.code, 'message': e.message},
      });
    } catch (error, stack) {
      log.warning('[AgentProtocolServer] $method failed', error, stack);
      _send(socket, {
        'id': id,
        'error': {'code': 'internal', 'message': error.toString()},
      });
    }
  }

  void _send(Socket socket, Map<String, Object?> message) {
    try {
      socket.writeln(jsonEncode(message));
    } catch (e) {
      _clients.remove(socket);
      socket.destroy();
      log.warning('[AgentProtocolServer] send failed: $e');
    }
  }

  static String _generateToken() {
    final random = Random.secure();
    final bytes = List<int>.generate(16, (_) => random.nextInt(256));
    return base64UrlEncode(bytes);
  }
}

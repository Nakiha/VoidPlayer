import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter/foundation.dart';

import '../../app_log.dart';

class AnalysisIpcTrack {
  final int fileId;
  final int slot;
  final String path;
  final String fileName;
  final String hash;
  final int durationUs;

  const AnalysisIpcTrack({
    required this.fileId,
    required this.slot,
    required this.path,
    required this.fileName,
    required this.hash,
    required this.durationUs,
  });

  Map<String, Object?> toJson() => {
    'fileId': fileId,
    'slot': slot,
    'path': path,
    'fileName': fileName,
    'hash': hash,
    'durationUs': durationUs,
  };

  static AnalysisIpcTrack fromJson(Map<String, Object?> json) {
    return AnalysisIpcTrack(
      fileId: (json['fileId'] as num?)?.toInt() ?? -1,
      slot: (json['slot'] as num?)?.toInt() ?? -1,
      path: json['path'] as String? ?? '',
      fileName: json['fileName'] as String? ?? '',
      hash: json['hash'] as String? ?? '',
      durationUs: (json['durationUs'] as num?)?.toInt() ?? 0,
    );
  }
}

class AnalysisIpcServer {
  ServerSocket? _server;
  final _clients = <Socket>{};
  String? _token;
  int _revision = 0;
  Map<String, Object?>? _lastSnapshot;

  bool get isStarted => _server != null;
  int? get port => _server?.port;
  String? get token => _token;

  Future<void> start() async {
    if (_server != null) return;
    _token = _generateToken();
    _server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    _server!.listen(
      _handleClient,
      onError: (Object error, StackTrace stack) {
        log.warning('[AnalysisIpcServer] accept failed: $error');
      },
    );
    log.info('[AnalysisIpcServer] listening on 127.0.0.1:${_server!.port}');
  }

  void publishTracks(List<AnalysisIpcTrack> tracks) {
    if (_server == null) return;
    _revision++;
    final message = <String, Object?>{
      'type': 'trackSnapshot',
      'revision': _revision,
      'tracks': [for (final track in tracks) track.toJson()],
    };
    _lastSnapshot = message;
    _sendToAll(message);
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
    _lastSnapshot = null;
  }

  void _handleClient(Socket socket) {
    var authorized = false;
    StreamSubscription<String>? subscription;
    subscription = socket
        .cast<List<int>>()
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen(
          (line) {
            Map<String, Object?> message;
            try {
              message = jsonDecode(line) as Map<String, Object?>;
            } catch (e) {
              log.warning('[AnalysisIpcServer] malformed message: $e');
              socket.destroy();
              return;
            }

            if (!authorized) {
              if (message['type'] != 'hello' || message['token'] != _token) {
                log.warning('[AnalysisIpcServer] rejected client');
                socket.destroy();
                return;
              }
              authorized = true;
              _clients.add(socket);
              log.info('[AnalysisIpcServer] analysis client connected');
              final snapshot = _lastSnapshot;
              if (snapshot != null) {
                _send(socket, snapshot);
              }
            }
          },
          onDone: () {
            _clients.remove(socket);
            unawaited(subscription?.cancel());
          },
          onError: (Object error, StackTrace stack) {
            _clients.remove(socket);
            log.warning('[AnalysisIpcServer] client error: $error');
            unawaited(subscription?.cancel());
          },
          cancelOnError: true,
        );
  }

  void _sendToAll(Map<String, Object?> message) {
    for (final client in List<Socket>.from(_clients)) {
      _send(client, message);
    }
  }

  void _send(Socket socket, Map<String, Object?> message) {
    try {
      socket.writeln(jsonEncode(message));
    } catch (e) {
      _clients.remove(socket);
      socket.destroy();
      log.warning('[AnalysisIpcServer] send failed: $e');
    }
  }

  static String _generateToken() {
    final random = Random.secure();
    final bytes = List<int>.generate(16, (_) => random.nextInt(256));
    return base64UrlEncode(bytes);
  }
}

class AnalysisIpcClient extends ChangeNotifier {
  final Socket _socket;
  late final StreamSubscription<String> _subscription;
  List<AnalysisIpcTrack> _tracks = const [];
  bool _hasSnapshot = false;

  AnalysisIpcClient._(this._socket);

  List<AnalysisIpcTrack> get tracks => List.unmodifiable(_tracks);
  bool get hasSnapshot => _hasSnapshot;

  static Future<AnalysisIpcClient?> connect({
    required int port,
    required String token,
  }) async {
    try {
      final socket = await Socket.connect(InternetAddress.loopbackIPv4, port);
      final client = AnalysisIpcClient._(socket);
      client._start(token);
      return client;
    } catch (e) {
      log.warning('[AnalysisIpcClient] connect failed: $e');
      return null;
    }
  }

  @override
  void dispose() {
    unawaited(_subscription.cancel());
    _socket.destroy();
    super.dispose();
  }

  void _start(String token) {
    _socket.writeln(
      jsonEncode(<String, Object?>{
        'type': 'hello',
        'role': 'analysis',
        'token': token,
      }),
    );
    _subscription = _socket
        .cast<List<int>>()
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen(
          _handleMessage,
          onDone: () {
            log.info('[AnalysisIpcClient] disconnected');
          },
          onError: (Object error, StackTrace stack) {
            log.warning('[AnalysisIpcClient] error: $error');
          },
          cancelOnError: true,
        );
  }

  void _handleMessage(String line) {
    Map<String, Object?> message;
    try {
      message = jsonDecode(line) as Map<String, Object?>;
    } catch (e) {
      log.warning('[AnalysisIpcClient] malformed message: $e');
      return;
    }
    if (message['type'] != 'trackSnapshot') return;
    final rawTracks = message['tracks'];
    if (rawTracks is! List) return;
    _tracks = [
      for (final rawTrack in rawTracks)
        if (rawTrack is Map)
          AnalysisIpcTrack.fromJson(Map<String, Object?>.from(rawTrack)),
    ];
    _hasSnapshot = true;
    notifyListeners();
  }
}

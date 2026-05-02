import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

import '../../../app_log.dart';
import 'analysis_ipc_models.dart';

class AnalysisIpcClient extends ChangeNotifier {
  final Socket _socket;
  late final StreamSubscription<String> _subscription;
  List<AnalysisIpcTrack> _tracks = const [];
  bool _hasSnapshot = false;
  bool _disposed = false;

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
    _disposed = true;
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
    if (_disposed) return;
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
    if (_disposed) return;
    notifyListeners();
  }
}

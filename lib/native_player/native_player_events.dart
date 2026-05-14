import 'dart:async';

import 'package:flutter/services.dart';

import 'native_player_protocol.dart';

enum NativePlayerEventType { seekPreviewPresented, trackError, unknown }

class NativePlayerEvent {
  final int schemaVersion;
  final int sequence;
  final String rawType;
  final NativePlayerEventType type;
  final int timestampUs;
  final int? requestId;
  final int? trackFileId;
  final int? ptsUs;
  final int? dtsUs;
  final int? targetPtsUs;
  final int? errorCode;

  const NativePlayerEvent({
    required this.schemaVersion,
    required this.sequence,
    required this.rawType,
    required this.type,
    required this.timestampUs,
    this.requestId,
    this.trackFileId,
    this.ptsUs,
    this.dtsUs,
    this.targetPtsUs,
    this.errorCode,
  });

  bool get hasPresentedFrame =>
      trackFileId != null &&
      ptsUs != null &&
      ptsUs! >= 0 &&
      dtsUs != null &&
      dtsUs != PresentedFrameTiming.noTimestampUs;

  factory NativePlayerEvent.fromMap(Map<dynamic, dynamic> map) {
    final rawType = map['type'] as String? ?? '';
    return NativePlayerEvent(
      schemaVersion: _asInt(map['schemaVersion']) ?? 0,
      sequence: _asInt(map['sequence']) ?? 0,
      rawType: rawType,
      type: switch (rawType) {
        'seekPreviewPresented' => NativePlayerEventType.seekPreviewPresented,
        'trackError' => NativePlayerEventType.trackError,
        _ => NativePlayerEventType.unknown,
      },
      timestampUs: _asInt(map['timestampUs']) ?? 0,
      requestId: _asInt(map['requestId']),
      trackFileId: _asInt(map['trackFileId']),
      ptsUs: _asInt(map['ptsUs']),
      dtsUs: _asInt(map['dtsUs']),
      targetPtsUs: _asInt(map['targetPtsUs']),
      errorCode: _asInt(map['errorCode']),
    );
  }

  static int? _asInt(Object? value) {
    if (value is int) return value;
    if (value is num) return value.toInt();
    return null;
  }
}

class NativePlayerEventStream {
  static const EventChannel _channel = EventChannel(
    NativePlayerChannel.eventsName,
  );

  const NativePlayerEventStream();

  Stream<NativePlayerEvent> get events => _channel
      .receiveBroadcastStream()
      .where((event) => event is Map)
      .map(
        (event) =>
            NativePlayerEvent.fromMap(Map<dynamic, dynamic>.from(event as Map)),
      )
      .where((event) => event.schemaVersion == 1);
}

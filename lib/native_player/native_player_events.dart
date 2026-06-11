import 'dart:async';

import 'package:flutter/services.dart';

import '../app_log.dart';
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
    final typeValue = map['type'];
    final rawType = typeValue is String ? typeValue : '';
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
  static final Stream<NativePlayerEvent> _events = _channel
      .receiveBroadcastStream()
      .where((event) => event is Map)
      .map(
        (event) =>
            NativePlayerEvent.fromMap(Map<dynamic, dynamic>.from(event as Map)),
      )
      .where((event) {
        if (event.schemaVersion != 1) {
          log.warning(
            'Dropping native player event with unsupported schema '
            '${event.schemaVersion}: ${event.rawType}',
          );
          return false;
        }
        if (event.type == NativePlayerEventType.unknown) {
          log.fine('Received unknown native player event: ${event.rawType}');
        }
        return true;
      })
      .asBroadcastStream();

  const NativePlayerEventStream();

  Stream<NativePlayerEvent> get events => _events;
}

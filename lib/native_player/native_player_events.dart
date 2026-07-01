import 'dart:async';

import 'package:flutter/services.dart';

import '../app_log.dart';
import 'native_player_protocol.dart';

enum NativePlayerEventType {
  seekPreviewPresented,
  trackError,
  rendererOwnedPresentationState,
  nativeCompositorState,
  playbackClock,
  unknown,
}

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
  final int? durationUs;
  final bool isPlaying;
  final double playbackSpeed;
  final int? errorCode;
  final bool rendererOwnedPresentationActive;
  final bool rendererOwnedRunnerLayerActive;
  final bool rendererOwnedRendererActive;
  final bool rendererOwnedPresentationRequested;
  final bool rendererOwnedEDROutputEnabled;
  final String rendererOwnedPresentationMode;
  final String rendererOwnedPresentationReason;
  final String rendererOwnedPresentationFailure;
  final bool nativeCompositorActive;
  final bool nativeCompositorRunnerLayerActive;
  final bool nativeCompositorRendererOwnedActive;
  final bool nativeCompositorRequested;
  final bool nativeCompositorEDREnabled;
  final String nativeCompositorMode;
  final String nativeCompositorReason;
  final String nativeCompositorFailure;
  final String nativeCompositorPhase;
  final int nativeCompositorSerial;

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
    this.durationUs,
    this.isPlaying = false,
    this.playbackSpeed = 1.0,
    this.errorCode,
    this.rendererOwnedPresentationActive = false,
    this.rendererOwnedRunnerLayerActive = false,
    this.rendererOwnedRendererActive = false,
    this.rendererOwnedPresentationRequested = false,
    this.rendererOwnedEDROutputEnabled = false,
    this.rendererOwnedPresentationMode = '',
    this.rendererOwnedPresentationReason = '',
    this.rendererOwnedPresentationFailure = '',
    this.nativeCompositorActive = false,
    this.nativeCompositorRunnerLayerActive = false,
    this.nativeCompositorRendererOwnedActive = false,
    this.nativeCompositorRequested = false,
    this.nativeCompositorEDREnabled = false,
    this.nativeCompositorMode = '',
    this.nativeCompositorReason = '',
    this.nativeCompositorFailure = '',
    this.nativeCompositorPhase = '',
    this.nativeCompositorSerial = 0,
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
        'rendererOwnedPresentationState' =>
          NativePlayerEventType.rendererOwnedPresentationState,
        'nativeCompositorState' => NativePlayerEventType.nativeCompositorState,
        'playbackClock' => NativePlayerEventType.playbackClock,
        _ => NativePlayerEventType.unknown,
      },
      timestampUs: _asInt(map['timestampUs']) ?? 0,
      requestId: _asInt(map['requestId']),
      trackFileId: _asInt(map['trackFileId']),
      ptsUs: _asInt(map['ptsUs']),
      dtsUs: _asInt(map['dtsUs']),
      targetPtsUs: _asInt(map['targetPtsUs']),
      durationUs: _asInt(map['durationUs']),
      isPlaying: _asBool(map['isPlaying']),
      playbackSpeed: _asDouble(map['playbackSpeed']) ?? 1.0,
      errorCode: _asInt(map['errorCode']),
      rendererOwnedPresentationActive: _boolValue(
        map,
        'rendererOwnedPresentationActive',
        fallbackKey: 'nativeCompositorActive',
      ),
      rendererOwnedRunnerLayerActive: _boolValue(
        map,
        'rendererOwnedRunnerLayerActive',
        fallbackKey: 'nativeCompositorRunnerLayerActive',
      ),
      rendererOwnedRendererActive: _boolValue(
        map,
        'rendererOwnedRendererActive',
        fallbackKey: 'nativeCompositorRendererOwnedActive',
      ),
      rendererOwnedPresentationRequested: _boolValue(
        map,
        'rendererOwnedPresentationRequested',
        fallbackKey: 'nativeCompositorRequested',
      ),
      rendererOwnedEDROutputEnabled: _boolValue(
        map,
        'rendererOwnedEDROutputEnabled',
        fallbackKey: 'nativeCompositorEDREnabled',
      ),
      rendererOwnedPresentationMode: _stringValue(
        map,
        'rendererOwnedPresentationMode',
        fallbackKey: 'nativeCompositorMode',
      ),
      rendererOwnedPresentationReason: _stringValue(
        map,
        'rendererOwnedPresentationReason',
        fallbackKey: 'nativeCompositorReason',
      ),
      rendererOwnedPresentationFailure: _stringValue(
        map,
        'rendererOwnedPresentationFailure',
        fallbackKey: 'nativeCompositorFailure',
      ),
      nativeCompositorActive: _boolValue(
        map,
        'rendererOwnedPresentationActive',
        fallbackKey: 'nativeCompositorActive',
      ),
      nativeCompositorRunnerLayerActive: _boolValue(
        map,
        'rendererOwnedRunnerLayerActive',
        fallbackKey: 'nativeCompositorRunnerLayerActive',
      ),
      nativeCompositorRendererOwnedActive: _boolValue(
        map,
        'rendererOwnedRendererActive',
        fallbackKey: 'nativeCompositorRendererOwnedActive',
      ),
      nativeCompositorRequested: _boolValue(
        map,
        'rendererOwnedPresentationRequested',
        fallbackKey: 'nativeCompositorRequested',
      ),
      nativeCompositorEDREnabled: _boolValue(
        map,
        'rendererOwnedEDROutputEnabled',
        fallbackKey: 'nativeCompositorEDREnabled',
      ),
      nativeCompositorMode: _stringValue(
        map,
        'rendererOwnedPresentationMode',
        fallbackKey: 'nativeCompositorMode',
      ),
      nativeCompositorReason: _stringValue(
        map,
        'rendererOwnedPresentationReason',
        fallbackKey: 'nativeCompositorReason',
      ),
      nativeCompositorFailure: _stringValue(
        map,
        'rendererOwnedPresentationFailure',
        fallbackKey: 'nativeCompositorFailure',
      ),
      nativeCompositorPhase: _asString(map['nativeCompositorPhase']),
      nativeCompositorSerial: _asInt(map['nativeCompositorSerial']) ?? 0,
    );
  }

  static int? _asInt(Object? value) {
    if (value is int) return value;
    if (value is num) return value.toInt();
    return null;
  }

  static bool _asBool(Object? value) {
    if (value is bool) return value;
    if (value is num) return value != 0;
    return false;
  }

  static double? _asDouble(Object? value) {
    if (value is double) return value;
    if (value is num) return value.toDouble();
    return null;
  }

  static String _asString(Object? value) {
    return value is String ? value : '';
  }

  static bool _boolValue(
    Map<dynamic, dynamic> map,
    String key, {
    required String fallbackKey,
  }) {
    if (map.containsKey(key)) return _asBool(map[key]);
    return _asBool(map[fallbackKey]);
  }

  static String _stringValue(
    Map<dynamic, dynamic> map,
    String key, {
    required String fallbackKey,
  }) {
    if (map.containsKey(key)) return _asString(map[key]);
    return _asString(map[fallbackKey]);
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

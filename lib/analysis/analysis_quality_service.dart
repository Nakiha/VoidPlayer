import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:isolate';

import 'package:path/path.dart' as p;

import '../app_log.dart';
import '../app_paths.dart';
import 'analysis_ffi.dart';
import 'analysis_manager.dart';

enum AnalysisQualityPhase { opening, decoding, finalizing }

final _qualityLogger = appLogger('QualityAnalysis');

class AnalysisQualityProgress {
  final AnalysisQualityPhase phase;
  final int decodedFrames;
  final int sampledFrames;
  final int? ptsUs;
  final int? durationUs;

  const AnalysisQualityProgress({
    required this.phase,
    required this.decodedFrames,
    required this.sampledFrames,
    required this.ptsUs,
    required this.durationUs,
  });

  double? get fraction {
    final duration = durationUs;
    final pts = ptsUs;
    if (duration == null || duration <= 0 || pts == null) return null;
    return (pts / duration).clamp(0.0, 1.0);
  }
}

class AnalysisQualityCancellationToken {
  bool _cancelled = false;
  void Function()? _listener;

  bool get isCancelled => _cancelled;

  void cancel() {
    if (_cancelled) return;
    _cancelled = true;
    _listener?.call();
  }

  void _attach(void Function() listener) {
    _listener = listener;
    if (_cancelled) listener();
  }

  void _detach() {
    _listener = null;
  }
}

class AnalysisQualityRequest {
  final String videoPath;
  final int sampleIntervalUs;
  final int maxSamples;
  final void Function(AnalysisQualityProgress progress)? onProgress;
  final AnalysisQualityCancellationToken? cancellationToken;

  const AnalysisQualityRequest({
    required this.videoPath,
    this.sampleIntervalUs = 1000000,
    this.maxSamples = 0,
    this.onProgress,
    this.cancellationToken,
  });
}

class AnalysisQualityException implements Exception {
  final String code;
  final String message;

  const AnalysisQualityException(this.code, this.message);

  @override
  String toString() => message;
}

abstract interface class AnalysisQualityDataSource {
  Future<AnalysisQualityReport> analyze(AnalysisQualityRequest request);
}

abstract interface class AnalysisQualityFrameDataSource {
  Future<Map<int, FrameInfo>> readCachedFrames(
    String videoPath,
    List<AnalysisQualitySample> samples,
  );
}

class NativeAnalysisQualityService
    implements AnalysisQualityDataSource, AnalysisQualityFrameDataSource {
  final String? cliExecutablePath;

  const NativeAnalysisQualityService({this.cliExecutablePath});

  @override
  Future<AnalysisQualityReport> analyze(AnalysisQualityRequest request) async {
    final operationId =
        'qa-${DateTime.now().microsecondsSinceEpoch.toRadixString(16)}';
    final backend = cliExecutablePath != null || Platform.isWindows
        ? 'cli'
        : 'ffi';
    final stopwatch = Stopwatch()..start();
    _qualityLogger.info(
      'analysis started operation=$operationId backend=$backend '
      'media=${p.basename(request.videoPath)} '
      'sample_interval_us=${request.sampleIntervalUs} '
      'max_samples=${request.maxSamples}',
    );
    try {
      // Isolate.run 要求闭包只捕获可跨 isolate 发送的对象；request 上挂着
      // UI 传入的 onProgress/cancellationToken（捕获了 widget State 等不可发送
      // 对象），因此这里必须先把原始参数提出来。
      final videoPath = request.videoPath;
      final sampleIntervalUs = request.sampleIntervalUs;
      final maxSamples = request.maxSamples;
      final report = backend == 'cli'
          ? await _analyzeWithCli(request)
          : await Isolate.run(
              () => AnalysisQualityNative.analyzeSync(
                videoPath,
                sampleIntervalUs: sampleIntervalUs,
                maxSamples: maxSamples,
              ),
            );
      _qualityLogger.info(
        'analysis completed operation=$operationId backend=$backend '
        'elapsed_ms=${stopwatch.elapsedMilliseconds} '
        'samples=${report.samples.length} tiles=${report.tileSamples.length} '
        'events=${report.events.length} truncated=${report.truncated}',
      );
      return report;
    } on AnalysisQualityException catch (error, stackTrace) {
      final message =
          'analysis ${error.code == 'cancelled' ? 'cancelled' : 'failed'} '
          'operation=$operationId backend=$backend '
          'elapsed_ms=${stopwatch.elapsedMilliseconds} code=${error.code}';
      if (error.code == 'cancelled') {
        _qualityLogger.info(message);
      } else {
        _qualityLogger.warning(message, error, stackTrace);
      }
      rethrow;
    } on Object catch (error, stackTrace) {
      _qualityLogger.warning(
        'analysis failed operation=$operationId backend=$backend '
        'elapsed_ms=${stopwatch.elapsedMilliseconds} '
        'code=unexpected_error',
        error,
        stackTrace,
      );
      rethrow;
    }
  }

  Future<AnalysisQualityReport> _analyzeWithCli(
    AnalysisQualityRequest request,
  ) async {
    if (request.videoPath.trim().isEmpty ||
        request.sampleIntervalUs < 0 ||
        request.sampleIntervalUs % 1000 != 0 ||
        request.maxSamples < 0) {
      throw const AnalysisQualityException(
        'invalid_request',
        'Quality request contains an invalid path or sampling interval.',
      );
    }
    final executable = cliExecutablePath ?? _resolveBundledCli();
    if (!File(executable).existsSync()) {
      throw AnalysisQualityException(
        'cli_unavailable',
        'Quality analyzer is unavailable: $executable',
      );
    }
    final requestId =
        'gui-$pid-${DateTime.now().microsecondsSinceEpoch.toRadixString(16)}';
    final arguments = <String>[
      'score-quality',
      '--input',
      request.videoPath,
      '--metrics',
      'all',
      '--regions',
      'full',
      '--tiles',
      'full',
      '--events',
      'candidates',
      '--backend',
      'cpu',
      '--sample-interval-ms',
      '${request.sampleIntervalUs ~/ 1000}',
      if (request.maxSamples > 0) ...['--max-samples', '${request.maxSamples}'],
      '--request-id',
      requestId,
      '--jsonl',
    ];

    final Process process;
    try {
      process = await Process.start(executable, arguments, runInShell: false);
    } on Object catch (error) {
      throw AnalysisQualityException(
        'cli_start_failed',
        'Unable to start quality analyzer: $error',
      );
    }
    final cancellation = request.cancellationToken;
    cancellation?._attach(() => process.kill());
    final stderrFuture = process.stderr.transform(utf8.decoder).join();
    final decoder = AnalysisQualityProtocolDecoder(
      expectedRequestId: requestId,
      onProgress: request.onProgress,
    );
    Object? streamError;
    try {
      await for (final line
          in process.stdout
              .transform(utf8.decoder)
              .transform(const LineSplitter())) {
        if (line.trim().isEmpty) continue;
        decoder.addLine(line);
      }
    } on Object catch (error) {
      streamError = error;
      process.kill();
    }
    try {
      final exitCode = await process.exitCode;
      final stderrText = (await stderrFuture).trim();
      if (cancellation?.isCancelled ?? false) {
        throw const AnalysisQualityException(
          'cancelled',
          'Quality analysis was cancelled.',
        );
      }
      if (streamError != null) {
        if (streamError is AnalysisQualityException) throw streamError;
        throw AnalysisQualityException(
          'protocol_error',
          'Unable to read quality analyzer output: $streamError',
        );
      }
      return decoder.finish(exitCode: exitCode, stderrText: stderrText);
    } finally {
      cancellation?._detach();
    }
  }

  String _resolveBundledCli() {
    final name = Platform.isWindows ? 'VoidPlayerCli.exe' : 'VoidPlayerCli';
    return p.join(AppPaths.current.exeDir, name);
  }

  @override
  Future<Map<int, FrameInfo>> readCachedFrames(
    String videoPath,
    List<AnalysisQualitySample> samples,
  ) {
    return AnalysisManager.instance.readCachedFrames(
      videoPath: videoPath,
      frameIndices: samples.map((sample) => sample.decodedFrameIndex),
      ptsUsByFrameIndex: {
        for (final sample in samples) sample.decodedFrameIndex: sample.ptsUs,
      },
    );
  }
}

class AnalysisQualityProtocolDecoder {
  final String expectedRequestId;
  final void Function(AnalysisQualityProgress progress)? onProgress;

  String? _sessionRequestId;
  String? _resultKey;
  Map<String, Object?>? _reportRecord;
  final List<AnalysisQualitySample> _samples = [];
  final List<AnalysisQualityTileSample> _tileSamples = [];
  final List<AnalysisQualityEvent> _events = [];
  AnalysisQualityException? _terminalError;
  var _nextProgressSequence = 0;
  var _eventRecordsStarted = false;
  var _expectsTileSamples = false;
  var _complete = false;

  AnalysisQualityProtocolDecoder({
    required this.expectedRequestId,
    this.onProgress,
  });

  void addLine(String line) {
    if (_complete || _terminalError != null) {
      throw const AnalysisQualityException(
        'protocol_error',
        'Quality analyzer emitted data after its terminal record.',
      );
    }
    final Object? decoded;
    try {
      decoded = jsonDecode(line);
    } on Object catch (error) {
      throw AnalysisQualityException(
        'protocol_error',
        'Quality analyzer emitted invalid JSON: $error',
      );
    }
    final record = _objectMap(decoded, 'record');
    final type = _string(record, 'type');
    switch (type) {
      case 'qualitySession':
        _acceptSession(record);
      case 'qualityProgress':
        _acceptProgress(record);
      case 'qualityReport':
        _acceptReport(record);
      case 'qualityFrameSample':
        _acceptSample(record);
      case 'qualityTileSample':
        _acceptTileSample(record);
      case 'qualityEvent':
        _acceptEvent(record);
      case 'qualityComplete':
        _acceptComplete(record);
      case 'qualityError':
        _acceptError(record);
      default:
        // Protocol v1 compatibility rule: unknown record types from a newer
        // minor-compatible producer are ignored, never parsed.
        break;
    }
  }

  AnalysisQualityReport finish({
    required int exitCode,
    String stderrText = '',
  }) {
    final terminalError = _terminalError;
    if (terminalError != null) throw terminalError;
    if (exitCode != 0) {
      throw AnalysisQualityException(
        'process_failed',
        stderrText.isEmpty
            ? 'Quality analyzer exited with code $exitCode.'
            : 'Quality analyzer exited with code $exitCode: $stderrText',
      );
    }
    if (!_complete || _reportRecord == null) {
      throw const AnalysisQualityException(
        'incomplete_result',
        'Quality analyzer ended before a complete result was committed.',
      );
    }
    return _parseReport(
      _reportRecord!,
      samples: List<AnalysisQualitySample>.unmodifiable(_samples),
      tileSamples: List<AnalysisQualityTileSample>.unmodifiable(_tileSamples),
      events: List<AnalysisQualityEvent>.unmodifiable(_events),
      resultKey: _resultKey,
    );
  }

  void _acceptSession(Map<String, Object?> record) {
    if (_sessionRequestId != null ||
        _reportRecord != null ||
        _samples.isNotEmpty ||
        _events.isNotEmpty) {
      _protocolFailure('duplicate or out-of-order qualitySession');
    }
    _requireProtocol(record);
    final requestId = _string(record, 'requestId');
    if (requestId != expectedRequestId) {
      _protocolFailure('qualitySession requestId does not match the request');
    }
    _sessionRequestId = requestId;
    _resultKey = _string(record, 'resultKey');
    final resultConfig = record['resultConfig'];
    if (resultConfig is Map) {
      final config = _objectMap(resultConfig, 'resultConfig');
      _expectsTileSamples = config['tileOutput'] == 'full';
    }
  }

  void _acceptProgress(Map<String, Object?> record) {
    _requireActiveSession(record);
    if (_reportRecord != null) {
      _protocolFailure('qualityProgress appeared after qualityReport');
    }
    final sequence = _integer(record, 'sequence');
    if (sequence != _nextProgressSequence++) {
      _protocolFailure('qualityProgress sequence is not contiguous');
    }
    final phase = switch (_string(record, 'phase')) {
      'opening' => AnalysisQualityPhase.opening,
      'decoding' => AnalysisQualityPhase.decoding,
      'finalizing' => AnalysisQualityPhase.finalizing,
      final value => throw AnalysisQualityException(
        'protocol_error',
        'Unknown quality progress phase "$value".',
      ),
    };
    onProgress?.call(
      AnalysisQualityProgress(
        phase: phase,
        decodedFrames: _integer(record, 'decodedFrames'),
        sampledFrames: _integer(record, 'sampledFrames'),
        ptsUs: _nullableInteger(record, 'ptsUs'),
        durationUs: _nullableInteger(record, 'durationUs'),
      ),
    );
  }

  void _acceptReport(Map<String, Object?> record) {
    _requireSession();
    if (_reportRecord != null || _samples.isNotEmpty || _events.isNotEmpty) {
      _protocolFailure('duplicate or out-of-order qualityReport');
    }
    if (_integer(record, 'schemaVersion') != 5 ||
        _string(record, 'schemaId') != 'quality-output-v5') {
      _protocolFailure('unsupported quality report schema');
    }
    _reportRecord = record;
  }

  void _acceptSample(Map<String, Object?> record) {
    _requireReport();
    if (_eventRecordsStarted) {
      _protocolFailure('qualityFrameSample appeared after qualityEvent');
    }
    if (_expectsTileSamples && _tileSamples.length != _samples.length) {
      _protocolFailure('qualityFrameSample appeared before its preceding tile');
    }
    if (_integer(record, 'schemaVersion') != 5 ||
        _string(record, 'schemaId') != 'quality-output-v5') {
      _protocolFailure('unsupported quality sample schema');
    }
    final sample = _parseSample(_objectMap(record['sample'], 'sample'));
    if (sample.sampleIndex != _samples.length) {
      _protocolFailure('qualityFrameSample indices are not contiguous');
    }
    _samples.add(sample);
  }

  void _acceptTileSample(Map<String, Object?> record) {
    _requireReport();
    if (_eventRecordsStarted) {
      _protocolFailure('qualityTileSample appeared after qualityEvent');
    }
    if (_integer(record, 'tileSchemaVersion') != 1 ||
        _string(record, 'tileSchemaId') != 'quality-tile-v1') {
      _protocolFailure('unsupported quality tile schema');
    }
    if (_string(record, 'requestId') != _sessionRequestId) {
      _protocolFailure(
        'qualityTileSample requestId does not match the session',
      );
    }
    final sample = _parseTileSample(record);
    if (sample.sampleIndex != _tileSamples.length ||
        sample.sampleIndex >= _samples.length ||
        _samples[sample.sampleIndex].decodedFrameIndex !=
            sample.decodedFrameIndex ||
        _samples[sample.sampleIndex].ptsUs != sample.ptsUs) {
      _protocolFailure('qualityTileSample does not match its frame sample');
    }
    _tileSamples.add(sample);
  }

  void _acceptEvent(Map<String, Object?> record) {
    _requireReport();
    if (_expectsTileSamples && _tileSamples.length != _samples.length) {
      _protocolFailure('qualityEvent appeared before all tile samples');
    }
    _eventRecordsStarted = true;
    if (_integer(record, 'eventSchemaVersion') != 1 ||
        _string(record, 'eventSchemaId') != 'quality-event-v1') {
      _protocolFailure('unsupported quality event schema');
    }
    if (_string(record, 'requestId') != _sessionRequestId) {
      _protocolFailure('qualityEvent requestId does not match the session');
    }
    _events.add(_parseEvent(record));
  }

  void _acceptComplete(Map<String, Object?> record) {
    _requireActiveSession(record);
    _requireReport();
    final tileSampleRecords =
        _nullableInteger(record, 'tileSampleRecords') ?? 0;
    if (_string(record, 'status') != 'success' ||
        _integer(record, 'reportRecords') != 1 ||
        _integer(record, 'frameSampleRecords') != _samples.length ||
        tileSampleRecords != _tileSamples.length ||
        (_expectsTileSamples && _tileSamples.length != _samples.length) ||
        _integer(record, 'eventRecords') != _events.length) {
      _protocolFailure('qualityComplete record counts do not match the stream');
    }
    _complete = true;
  }

  void _acceptError(Map<String, Object?> record) {
    if (_sessionRequestId != null) {
      _requireActiveSession(record);
    }
    _terminalError = AnalysisQualityException(
      _string(record, 'code'),
      _string(record, 'message'),
    );
  }

  void _requireSession() {
    if (_sessionRequestId == null) {
      _protocolFailure('quality record appeared before qualitySession');
    }
  }

  void _requireReport() {
    _requireSession();
    if (_reportRecord == null) {
      _protocolFailure('quality payload appeared before qualityReport');
    }
  }

  void _requireActiveSession(Map<String, Object?> record) {
    _requireSession();
    _requireProtocol(record);
    if (_string(record, 'requestId') != _sessionRequestId) {
      _protocolFailure('quality record requestId does not match the session');
    }
  }

  void _requireProtocol(Map<String, Object?> record) {
    if (_integer(record, 'protocolVersion') != 1) {
      _protocolFailure('unsupported quality process protocol');
    }
  }

  Never _protocolFailure(String message) {
    throw AnalysisQualityException('protocol_error', message);
  }
}

AnalysisQualityReport _parseReport(
  Map<String, Object?> record, {
  required List<AnalysisQualitySample> samples,
  required List<AnalysisQualityTileSample> tileSamples,
  required List<AnalysisQualityEvent> events,
  required String? resultKey,
}) {
  final video = _objectMap(record['video'], 'video');
  final sampling = _objectMap(record['sampling'], 'sampling');
  final metrics = _objectMap(record['metrics'], 'metrics');
  return AnalysisQualityReport(
    schemaVersion: _integer(record, 'schemaVersion'),
    metricVersion: _string(record, 'metricVersion'),
    resultKey: resultKey,
    videoWidth: _integer(video, 'width'),
    videoHeight: _integer(video, 'height'),
    bitDepth: _integer(video, 'bitDepth'),
    sampleIntervalUs: _integer(sampling, 'intervalUs'),
    maxSamples: _nullableInteger(sampling, 'maxSamples') ?? 0,
    truncated: _boolean(sampling, 'truncated'),
    unsupportedPixelFrames: _integer(sampling, 'unsupportedPixelFrames'),
    distributions: Map.unmodifiable({
      for (final metric in AnalysisQualityMetric.values)
        metric: _parseDistribution(
          _objectMap(
            _objectMap(metrics[metric.name], metric.name)['distribution'],
            '${metric.name}.distribution',
          ),
        ),
    }),
    samples: samples,
    tileSamples: tileSamples,
    events: events,
  );
}

AnalysisQualityDistribution _parseDistribution(Map<String, Object?> value) {
  return AnalysisQualityDistribution(
    count: _integer(value, 'count'),
    mean: _number(value, 'mean'),
    p95: _number(value, 'p95'),
    maximum: _number(value, 'max'),
  );
}

AnalysisQualitySample _parseSample(Map<String, Object?> value) {
  return AnalysisQualitySample(
    sampleIndex: _integer(value, 'sampleIndex'),
    decodedFrameIndex: _integer(value, 'decodedFrameIndex'),
    ptsUs: _integer(value, 'ptsUs'),
    blockiness: _number(value, 'blockiness'),
    banding: _number(value, 'banding'),
    blur: _number(value, 'blur'),
    noise: _number(value, 'noise'),
    flicker: _nullableNumber(value, 'flicker'),
    averageQp: _nullableNumber(value, 'averageQp'),
    spatialRegions: List.unmodifiable(
      _objectList(value, 'spatialRegions').map(_parseSpatialRegion),
    ),
  );
}

AnalysisQualityTileSample _parseTileSample(Map<String, Object?> value) {
  final grid = _objectMap(value['grid'], 'grid');
  if (_string(grid, 'coordinateSpace') != 'decodedLumaPixels' ||
      _string(grid, 'order') != 'rowMajor' ||
      _string(grid, 'partition') != 'balanced') {
    throw const AnalysisQualityException(
      'protocol_error',
      'Unsupported quality tile grid semantics.',
    );
  }
  final columns = _integer(grid, 'columns');
  final rows = _integer(grid, 'rows');
  final frameWidth = _integer(grid, 'frameWidth');
  final frameHeight = _integer(grid, 'frameHeight');
  final targetTileWidth = _integer(grid, 'targetTileWidth');
  final targetTileHeight = _integer(grid, 'targetTileHeight');
  if (columns <= 0 ||
      rows <= 0 ||
      frameWidth <= 0 ||
      frameHeight <= 0 ||
      targetTileWidth <= 0 ||
      targetTileHeight <= 0 ||
      columns > frameWidth ||
      rows > frameHeight) {
    throw const AnalysisQualityException(
      'protocol_error',
      'Quality tile grid dimensions are invalid.',
    );
  }
  final rawMetrics = _objectMap(value['metrics'], 'metrics');
  final metrics = <AnalysisQualityMetric, AnalysisQualityTileMetricData>{};
  for (final metric in AnalysisQualityMetric.values) {
    final rawMetric = rawMetrics[metric.name];
    if (rawMetric == null) continue;
    final metricData = _objectMap(rawMetric, metric.name);
    final available = _boolean(metricData, 'available');
    final rawValues = metricData['values'];
    List<double?>? values;
    if (rawValues != null) {
      if (rawValues is! List || rawValues.length != columns * rows) {
        throw AnalysisQualityException(
          'protocol_error',
          '${metric.name} tile values do not match the grid.',
        );
      }
      values = List<double?>.unmodifiable(
        rawValues.map((value) {
          if (value == null) return null;
          if (value is num && value >= 0 && value <= 1) {
            return value.toDouble();
          }
          throw AnalysisQualityException(
            'protocol_error',
            '${metric.name} tile values must be in [0, 1] or null.',
          );
        }),
      );
    }
    if (available != (values != null && values.any((value) => value != null))) {
      throw AnalysisQualityException(
        'protocol_error',
        '${metric.name} tile availability does not match its values.',
      );
    }
    metrics[metric] = AnalysisQualityTileMetricData(
      available: available,
      algorithm: _string(metricData, 'algorithm'),
      values: values,
    );
  }
  if (metrics.isEmpty) {
    throw const AnalysisQualityException(
      'protocol_error',
      'Quality tile sample contains no metrics.',
    );
  }
  return AnalysisQualityTileSample(
    sampleIndex: _integer(value, 'sampleIndex'),
    decodedFrameIndex: _integer(value, 'decodedFrameIndex'),
    ptsUs: _integer(value, 'ptsUs'),
    tileMetricVersion: _string(value, 'tileMetricVersion'),
    frameWidth: frameWidth,
    frameHeight: frameHeight,
    targetTileWidth: targetTileWidth,
    targetTileHeight: targetTileHeight,
    columns: columns,
    rows: rows,
    metrics: Map.unmodifiable(metrics),
  );
}

AnalysisQualityEvent _parseEvent(Map<String, Object?> value) {
  final classification = switch (_string(value, 'classification')) {
    'relativeOutlier' => AnalysisQualityEventClassification.relativeOutlier,
    'spatialCandidate' => AnalysisQualityEventClassification.spatialCandidate,
    final name => throw AnalysisQualityException(
      'protocol_error',
      'Unknown quality event classification "$name".',
    ),
  };
  final regionValue = value['region'];
  return AnalysisQualityEvent(
    eventId: _string(value, 'eventId'),
    metric: _metric(_string(value, 'metric')),
    classification: classification,
    startPtsUs: _integer(value, 'startPtsUs'),
    endPtsUs: _integer(value, 'endPtsUs'),
    peakPtsUs: _integer(value, 'peakPtsUs'),
    startSampleIndex: _integer(value, 'startSampleIndex'),
    endSampleIndex: _integer(value, 'endSampleIndex'),
    peakSampleIndex: _integer(value, 'peakSampleIndex'),
    peakScore: _number(value, 'peakScore'),
    evidenceSampleCount: _integer(value, 'evidenceSampleCount'),
    threshold: _number(_objectMap(value['threshold'], 'threshold'), 'value'),
    region: regionValue == null
        ? null
        : _parseSpatialRegion(_objectMap(regionValue, 'region')),
  );
}

AnalysisQualitySpatialRegion _parseSpatialRegion(Map<String, Object?> value) {
  final rect = _objectMap(value['rect'], 'rect');
  final pixelRect = _objectMap(value['pixelRect'], 'pixelRect');
  return AnalysisQualitySpatialRegion(
    score: _number(value, 'score'),
    detectionThreshold: _number(value, 'detectionThreshold'),
    x: _number(rect, 'x'),
    y: _number(rect, 'y'),
    width: _number(rect, 'width'),
    height: _number(rect, 'height'),
    pixelX: _integer(pixelRect, 'x'),
    pixelY: _integer(pixelRect, 'y'),
    pixelWidth: _integer(pixelRect, 'width'),
    pixelHeight: _integer(pixelRect, 'height'),
  );
}

AnalysisQualityMetric _metric(String name) => switch (name) {
  'blockiness' => AnalysisQualityMetric.blockiness,
  'banding' => AnalysisQualityMetric.banding,
  'blur' => AnalysisQualityMetric.blur,
  'noise' => AnalysisQualityMetric.noise,
  'flicker' => AnalysisQualityMetric.flicker,
  _ => throw AnalysisQualityException(
    'protocol_error',
    'Unknown quality metric "$name".',
  ),
};

Map<String, Object?> _objectMap(Object? value, String name) {
  if (value is! Map) {
    throw AnalysisQualityException(
      'protocol_error',
      '$name must be an object.',
    );
  }
  return value.map((key, item) => MapEntry(key.toString(), item));
}

List<Map<String, Object?>> _objectList(
  Map<String, Object?> value,
  String name,
) {
  final raw = value[name];
  if (raw is! List) {
    throw AnalysisQualityException('protocol_error', '$name must be a list.');
  }
  return raw.map((item) => _objectMap(item, name)).toList(growable: false);
}

String _string(Map<String, Object?> value, String name) {
  final raw = value[name];
  if (raw is String) return raw;
  throw AnalysisQualityException('protocol_error', '$name must be a string.');
}

int _integer(Map<String, Object?> value, String name) {
  final raw = value[name];
  if (raw is int) return raw;
  throw AnalysisQualityException('protocol_error', '$name must be an integer.');
}

int? _nullableInteger(Map<String, Object?> value, String name) {
  final raw = value[name];
  if (raw == null || raw is int) return raw as int?;
  throw AnalysisQualityException(
    'protocol_error',
    '$name must be an integer or null.',
  );
}

double _number(Map<String, Object?> value, String name) {
  final raw = value[name];
  if (raw is num) return raw.toDouble();
  throw AnalysisQualityException('protocol_error', '$name must be a number.');
}

double? _nullableNumber(Map<String, Object?> value, String name) {
  final raw = value[name];
  if (raw == null) return null;
  if (raw is num) return raw.toDouble();
  throw AnalysisQualityException(
    'protocol_error',
    '$name must be a number or null.',
  );
}

bool _boolean(Map<String, Object?> value, String name) {
  final raw = value[name];
  if (raw is bool) return raw;
  throw AnalysisQualityException('protocol_error', '$name must be a boolean.');
}

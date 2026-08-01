import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_ffi.dart';
import 'package:void_player/analysis/analysis_quality_service.dart';

void main() {
  group('AnalysisQualityProtocolDecoder', () {
    test('decodes a complete stream with samples and events', () {
      final progress = <AnalysisQualityProgress>[];
      final decoder = AnalysisQualityProtocolDecoder(
        expectedRequestId: 'q-1',
        onProgress: progress.add,
      );

      decoder.addLines(const [
        _tileSessionLine,
        '{"type":"qualityProgress","protocolVersion":1,"requestId":"q-1",'
            '"sequence":0,"phase":"opening","packetCount":0,"packetBytes":0,'
            '"decodedFrames":0,"sampledFrames":0,"ptsUs":null,'
            '"durationUs":4000000}',
        '{"type":"qualityProgress","protocolVersion":1,"requestId":"q-1",'
            '"sequence":1,"phase":"decoding","packetCount":60,'
            '"packetBytes":1024,"decodedFrames":58,"sampledFrames":2,'
            '"ptsUs":2000000,"durationUs":4000000}',
        '{"type":"futureRecord","protocolVersion":1,"requestId":"q-1"}',
      ]);
      decoder.addLine(_reportLine);
      decoder.addLine(_sampleLine(0, 0, 0));
      decoder.addLine(_tileLine(0, 0, 0, const [0.1, 0.8]));
      decoder.addLine(_sampleLine(1, 30, 1000000));
      decoder.addLine(_tileLine(1, 30, 1000000, const [0.7, 0.2]));
      decoder.addLine(_spatialEventLine);
      decoder.addLine(_relativeEventLine);
      decoder.addLine(
        '{"type":"qualityComplete","protocolVersion":1,"requestId":"q-1",'
        '"status":"success","reportRecords":1,"frameSampleRecords":2,'
        '"tileSampleRecords":2,"eventRecords":2}',
      );

      final report = decoder.finish(exitCode: 0);

      expect(progress, hasLength(2));
      expect(progress.first.phase, AnalysisQualityPhase.opening);
      expect(progress.first.fraction, isNull);
      expect(progress.last.phase, AnalysisQualityPhase.decoding);
      expect(progress.last.fraction, 0.5);

      expect(report.schemaVersion, 5);
      expect(report.metricVersion, 'quality-demo-v5');
      expect(report.resultKey, 'fnv1a64:abc');
      expect(report.hasEventCandidates, isTrue);
      expect(report.videoWidth, 1920);
      expect(report.videoHeight, 1080);
      expect(report.truncated, isFalse);
      expect(report.distributions[AnalysisQualityMetric.banding]!.p95, 0.2);

      expect(report.samples, hasLength(2));
      expect(report.samples[1].ptsUs, 1000000);
      expect(report.samples[1].spatialRegions, hasLength(1));
      expect(report.samples[1].spatialRegions.single.pixelWidth, 576);

      expect(report.tileSamples, hasLength(2));
      final firstTileSample = report.tileSamples.first;
      expect(firstTileSample.columns, 2);
      expect(firstTileSample.rows, 1);
      expect(
        firstTileSample.metrics[AnalysisQualityMetric.blockiness]!.algorithm,
        'blockiness-period-8-16-local-v1',
      );
      final peak = firstTileSample.strongestTileFor(
        AnalysisQualityMetric.blockiness,
      );
      expect(peak, isNotNull);
      expect(peak!.column, 1);
      expect(peak.x, 0.5);
      expect(peak.width, 0.5);
      expect(peak.value, 0.8);

      expect(report.events, hasLength(2));
      final spatial = report.events.first;
      expect(spatial.eventId, 'evt-1');
      expect(spatial.metric, AnalysisQualityMetric.banding);
      expect(
        spatial.classification,
        AnalysisQualityEventClassification.spatialCandidate,
      );
      expect(spatial.peakPtsUs, 1000000);
      expect(spatial.threshold, 0.5);
      expect(spatial.region, isNotNull);
      expect(spatial.region!.x, 0.1);
      expect(spatial.region!.pixelHeight, 432);
      final relative = report.events.last;
      expect(
        relative.classification,
        AnalysisQualityEventClassification.relativeOutlier,
      );
      expect(relative.region, isNull);
    });

    test('throws the terminal qualityError code and message', () {
      final decoder = AnalysisQualityProtocolDecoder(expectedRequestId: 'q-1');
      decoder.addLine(
        '{"type":"qualityError","schemaVersion":5,'
        '"schemaId":"quality-output-v5","code":"analysis_failed",'
        '"message":"decode broke"}',
      );
      expect(
        () => decoder.finish(exitCode: 2),
        _throwsQualityException('analysis_failed', 'decode broke'),
      );
    });

    test('rejects a stream that ends before qualityComplete', () {
      final decoder = AnalysisQualityProtocolDecoder(expectedRequestId: 'q-1');
      decoder.addLine(_sessionLine);
      decoder.addLine(_reportLine);
      expect(
        () => decoder.finish(exitCode: 0),
        _throwsQualityException('incomplete_result'),
      );
    });

    test('rejects a nonzero exit without a terminal record', () {
      final decoder = AnalysisQualityProtocolDecoder(expectedRequestId: 'q-1');
      decoder.addLine(_sessionLine);
      expect(
        () => decoder.finish(exitCode: 1, stderrText: 'boom'),
        _throwsQualityException('process_failed', 'boom'),
      );
    });

    test('rejects a session requestId that does not match the request', () {
      final decoder = AnalysisQualityProtocolDecoder(expectedRequestId: 'q-1');
      expect(
        () => decoder.addLine(
          '{"type":"qualitySession","protocolVersion":1,"requestId":"q-2",'
          '"resultKey":"fnv1a64:abc"}',
        ),
        _throwsQualityException('protocol_error'),
      );
    });

    test('rejects out-of-order payload records', () {
      final decoder = AnalysisQualityProtocolDecoder(expectedRequestId: 'q-1');
      decoder.addLine(_sessionLine);
      expect(
        () => decoder.addLine(_sampleLine(0, 0, 0)),
        _throwsQualityException('protocol_error'),
      );
    });

    test('rejects non-contiguous progress sequences and sample indices', () {
      final decoder = AnalysisQualityProtocolDecoder(expectedRequestId: 'q-1');
      decoder.addLine(_sessionLine);
      expect(
        () => decoder.addLine(
          '{"type":"qualityProgress","protocolVersion":1,"requestId":"q-1",'
          '"sequence":3,"phase":"decoding","packetCount":1,"packetBytes":1,'
          '"decodedFrames":1,"sampledFrames":0,"ptsUs":null,'
          '"durationUs":null}',
        ),
        _throwsQualityException('protocol_error'),
      );

      final samples = AnalysisQualityProtocolDecoder(expectedRequestId: 'q-1');
      samples.addLine(_sessionLine);
      samples.addLine(_reportLine);
      expect(
        () => samples.addLine(_sampleLine(1, 30, 1000000)),
        _throwsQualityException('protocol_error'),
      );
    });

    test('rejects complete counts that do not match the stream', () {
      final decoder = AnalysisQualityProtocolDecoder(expectedRequestId: 'q-1');
      decoder.addLine(_sessionLine);
      decoder.addLine(_reportLine);
      decoder.addLine(_sampleLine(0, 0, 0));
      expect(
        () => decoder.addLine(
          '{"type":"qualityComplete","protocolVersion":1,"requestId":"q-1",'
          '"status":"success","reportRecords":1,"frameSampleRecords":2,'
          '"eventRecords":0}',
        ),
        _throwsQualityException('protocol_error'),
      );
    });

    test('rejects a tile-enabled stream that omits tile evidence', () {
      final decoder = AnalysisQualityProtocolDecoder(expectedRequestId: 'q-1');
      decoder.addLine(_tileSessionLine);
      decoder.addLine(_reportLine);
      decoder.addLine(_sampleLine(0, 0, 0));
      expect(
        () => decoder.addLine(
          '{"type":"qualityComplete","protocolVersion":1,'
          '"requestId":"q-1","status":"success","reportRecords":1,'
          '"frameSampleRecords":1,"tileSampleRecords":0,'
          '"eventRecords":0}',
        ),
        _throwsQualityException('protocol_error'),
      );
    });

    test('rejects tile grids that cannot map to a decoded frame', () {
      final decoder = AnalysisQualityProtocolDecoder(expectedRequestId: 'q-1');
      decoder.addLine(_tileSessionLine);
      decoder.addLine(_reportLine);
      decoder.addLine(_sampleLine(0, 0, 0));
      final invalidGrid = _tileLine(0, 0, 0, const [
        0.1,
        0.8,
      ]).replaceFirst('"frameWidth":1920', '"frameWidth":0');
      expect(
        () => decoder.addLine(invalidGrid),
        _throwsQualityException('protocol_error', 'grid dimensions'),
      );
    });

    test('rejects records after the terminal record', () {
      final decoder = AnalysisQualityProtocolDecoder(expectedRequestId: 'q-1');
      decoder.addLine(_sessionLine);
      decoder.addLine(_reportLine);
      decoder.addLine(
        '{"type":"qualityComplete","protocolVersion":1,"requestId":"q-1",'
        '"status":"success","reportRecords":1,"frameSampleRecords":0,'
        '"eventRecords":0}',
      );
      expect(
        () => decoder.addLine(_reportLine),
        _throwsQualityException('protocol_error'),
      );
    });

    test('rejects unsupported protocol versions', () {
      final decoder = AnalysisQualityProtocolDecoder(expectedRequestId: 'q-1');
      expect(
        () => decoder.addLine(
          '{"type":"qualitySession","protocolVersion":2,"requestId":"q-1",'
          '"resultKey":"fnv1a64:abc"}',
        ),
        _throwsQualityException('protocol_error'),
      );
    });
  });

  group('NativeAnalysisQualityService CLI request validation', () {
    test('rejects invalid sampling arguments before launching', () {
      const service = NativeAnalysisQualityService(
        cliExecutablePath: 'missing-cli',
      );
      expect(
        () => service.analyze(
          const AnalysisQualityRequest(
            videoPath: 'video.mp4',
            sampleIntervalUs: 1500,
          ),
        ),
        _throwsQualityException('invalid_request'),
      );
    });

    test('fails closed when the CLI executable is unavailable', () {
      const service = NativeAnalysisQualityService(
        cliExecutablePath: 'missing-cli',
      );
      expect(
        () => service.analyze(
          const AnalysisQualityRequest(videoPath: 'video.mp4'),
        ),
        _throwsQualityException('cli_unavailable', 'missing-cli'),
      );
    });
  });
}

const _sessionLine =
    '{"type":"qualitySession","protocolVersion":1,"requestId":"q-1",'
    '"resultKey":"fnv1a64:abc"}';

const _tileSessionLine =
    '{"type":"qualitySession","protocolVersion":1,"requestId":"q-1",'
    '"resultConfig":{"tileOutput":"full"},'
    '"resultKey":"fnv1a64:abc"}';

const _reportLine =
    '{"type":"qualityReport","schemaVersion":5,'
    '"schemaId":"quality-output-v5","metricVersion":"quality-demo-v5",'
    '"video":{"width":1920,"height":1080,"bitDepth":8},'
    '"sampling":{"intervalUs":1000000,"maxSamples":600,"truncated":false,'
    '"unsupportedPixelFrames":0},'
    '"metrics":{'
    '"blockiness":{"distribution":{"count":2,"mean":0.1,"p95":0.1,'
    '"max":0.1}},'
    '"banding":{"distribution":{"count":2,"mean":0.15,"p95":0.2,"max":0.4}},'
    '"blur":{"distribution":{"count":2,"mean":0.1,"p95":0.1,"max":0.1}},'
    '"noise":{"distribution":{"count":2,"mean":0.1,"p95":0.1,"max":0.1}},'
    '"flicker":{"distribution":{"count":2,"mean":0.1,"p95":0.1,"max":0.1}}'
    '}}';

String _sampleLine(int sampleIndex, int decodedFrameIndex, int ptsUs) {
  final regions = sampleIndex == 1
      ? '[{"metric":"banding","score":0.62,"detectionThreshold":0.5,'
            '"rect":{"x":0.1,"y":0.2,"width":0.3,"height":0.4},'
            '"pixelRect":{"x":192,"y":216,"width":576,"height":432}}]'
      : '[]';
  return '{"type":"qualityFrameSample","schemaVersion":5,'
      '"schemaId":"quality-output-v5","sample":{'
      '"sampleIndex":$sampleIndex,'
      '"decodedFrameIndex":$decodedFrameIndex,'
      '"ptsUs":$ptsUs,'
      '"blockiness":0.1,"banding":0.2,"blur":0.1,"noise":0.1,'
      '"flicker":0.1,"averageQp":null,'
      '"spatialRegions":$regions}}';
}

String _tileLine(
  int sampleIndex,
  int decodedFrameIndex,
  int ptsUs,
  List<double> values,
) {
  return '{"type":"qualityTileSample","tileSchemaVersion":1,'
      '"tileSchemaId":"quality-tile-v1",'
      '"tileMetricVersion":"quality-tile-metrics-v1",'
      '"metricVersion":"quality-demo-v5","requestId":"q-1",'
      '"sampleIndex":$sampleIndex,'
      '"decodedFrameIndex":$decodedFrameIndex,"ptsUs":$ptsUs,'
      '"grid":{"coordinateSpace":"decodedLumaPixels",'
      '"order":"rowMajor","partition":"balanced",'
      '"frameWidth":1920,"frameHeight":1080,'
      '"targetTileWidth":64,"targetTileHeight":64,'
      '"columns":2,"rows":1},'
      '"metrics":{"blockiness":{"available":true,'
      '"algorithm":"blockiness-period-8-16-local-v1",'
      '"values":[${values.join(',')}]}}}';
}

const _spatialEventLine =
    '{"type":"qualityEvent","eventSchemaVersion":1,'
    '"eventSchemaId":"quality-event-v1","requestId":"q-1",'
    '"eventId":"evt-1","metric":"banding",'
    '"classification":"spatialCandidate",'
    '"startPtsUs":500000,"endPtsUs":1500000,"peakPtsUs":1000000,'
    '"startSampleIndex":0,"endSampleIndex":1,"peakSampleIndex":1,'
    '"peakScore":0.62,"evidenceSampleCount":2,'
    '"threshold":{"kind":"spatialDetection","value":0.5},'
    '"region":{"metric":"banding","score":0.62,"detectionThreshold":0.5,'
    '"rect":{"x":0.1,"y":0.2,"width":0.3,"height":0.4},'
    '"pixelRect":{"x":192,"y":216,"width":576,"height":432}}}';

const _relativeEventLine =
    '{"type":"qualityEvent","eventSchemaVersion":1,'
    '"eventSchemaId":"quality-event-v1","requestId":"q-1",'
    '"eventId":"evt-2","metric":"banding",'
    '"classification":"relativeOutlier",'
    '"startPtsUs":2000000,"endPtsUs":3000000,"peakPtsUs":3000000,'
    '"startSampleIndex":2,"endSampleIndex":3,"peakSampleIndex":3,'
    '"peakScore":0.55,"evidenceSampleCount":6,'
    '"threshold":{"kind":"robustRelative","value":0.4},'
    '"region":null}';

Matcher _throwsQualityException(String code, [String? messageFragment]) {
  return throwsA(
    isA<AnalysisQualityException>()
        .having((error) => error.code, 'code', code)
        .having(
          (error) => error.message,
          'message',
          messageFragment == null ? isNotEmpty : contains(messageFragment),
        ),
  );
}

extension on AnalysisQualityProtocolDecoder {
  void addLines(List<String> lines) {
    for (final line in lines) {
      addLine(line);
    }
  }
}

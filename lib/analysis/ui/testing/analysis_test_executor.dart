import 'dart:math' as math;

import '../../nalu_types.dart';
import '../widgets/analysis_frame_utils.dart';
import 'analysis_test_host.dart';

enum AnalysisTestCommandType {
  waitLoaded,
  assertLoaded,
  assertCounts,
  assertMinCounts,
  assertCodec,
  assertNaluName,
  assertSelectedFrame,
  assertSelectedFrameVisible,
  assertReferenceEdges,
  assertReferenceMaxLayer,
  setTab,
  assertTab,
  setOrder,
  assertOrder,
  setLayerMode,
  assertLayerMode,
  setChartWindow,
  selectNalu,
  assertDetailVisible,
  activateFrame,
  assertCurrentFrame,
}

class AnalysisTestCommand {
  final AnalysisTestCommandType type;
  final List<String> args;

  const AnalysisTestCommand(this.type, this.args);

  String stringArg(int index, {String? defaultValue}) {
    if (index < args.length && args[index].isNotEmpty) return args[index];
    if (defaultValue != null) return defaultValue;
    throw ArgumentError('Missing argument $index for $type');
  }

  int intArg(int index, {int? defaultValue}) {
    if (index < args.length && args[index].isNotEmpty) {
      return int.parse(args[index]);
    }
    if (defaultValue != null) return defaultValue;
    throw ArgumentError('Missing integer argument $index for $type');
  }

  double doubleArg(int index, {double? defaultValue}) {
    if (index < args.length && args[index].isNotEmpty) {
      return double.parse(args[index]);
    }
    if (defaultValue != null) return defaultValue;
    throw ArgumentError('Missing number argument $index for $type');
  }
}

AnalysisTestCommand? tryParseAnalysisTestCommand(
  String command,
  List<String> args,
) {
  final type = switch (command.toUpperCase()) {
    'WAIT_ANALYSIS_LOADED' => AnalysisTestCommandType.waitLoaded,
    'ASSERT_ANALYSIS_LOADED' => AnalysisTestCommandType.assertLoaded,
    'ASSERT_ANALYSIS_COUNTS' => AnalysisTestCommandType.assertCounts,
    'ASSERT_ANALYSIS_MIN_COUNTS' => AnalysisTestCommandType.assertMinCounts,
    'ASSERT_ANALYSIS_CODEC' => AnalysisTestCommandType.assertCodec,
    'ASSERT_ANALYSIS_NALU_NAME' => AnalysisTestCommandType.assertNaluName,
    'ASSERT_ANALYSIS_SELECTED_FRAME' =>
      AnalysisTestCommandType.assertSelectedFrame,
    'ASSERT_ANALYSIS_SELECTED_FRAME_VISIBLE' =>
      AnalysisTestCommandType.assertSelectedFrameVisible,
    'ASSERT_ANALYSIS_REFERENCE_EDGES' =>
      AnalysisTestCommandType.assertReferenceEdges,
    'ASSERT_ANALYSIS_REFERENCE_MAX_LAYER' =>
      AnalysisTestCommandType.assertReferenceMaxLayer,
    'SET_ANALYSIS_TAB' => AnalysisTestCommandType.setTab,
    'ASSERT_ANALYSIS_TAB' => AnalysisTestCommandType.assertTab,
    'SET_ANALYSIS_ORDER' => AnalysisTestCommandType.setOrder,
    'ASSERT_ANALYSIS_ORDER' => AnalysisTestCommandType.assertOrder,
    'SET_ANALYSIS_LAYER_MODE' => AnalysisTestCommandType.setLayerMode,
    'ASSERT_ANALYSIS_LAYER_MODE' => AnalysisTestCommandType.assertLayerMode,
    'SET_ANALYSIS_CHART_WINDOW' => AnalysisTestCommandType.setChartWindow,
    'SELECT_ANALYSIS_NALU' => AnalysisTestCommandType.selectNalu,
    'ASSERT_ANALYSIS_DETAIL_VISIBLE' =>
      AnalysisTestCommandType.assertDetailVisible,
    'ACTIVATE_ANALYSIS_FRAME' => AnalysisTestCommandType.activateFrame,
    'ASSERT_ANALYSIS_CURRENT_FRAME' =>
      AnalysisTestCommandType.assertCurrentFrame,
    _ => null,
  };
  return type == null ? null : AnalysisTestCommand(type, args);
}

class AnalysisTestExecutor {
  final AnalysisTestHost host;

  const AnalysisTestExecutor(this.host);

  Future<void> execute(AnalysisTestCommand command) async {
    switch (command.type) {
      case AnalysisTestCommandType.waitLoaded:
        await _waitForLoaded(
          Duration(milliseconds: command.intArg(0, defaultValue: 10000)),
        );
      case AnalysisTestCommandType.assertLoaded:
        _assertLoaded();
      case AnalysisTestCommandType.assertCounts:
        _assertCounts(command.intArg(0), command.intArg(1), command.intArg(2));
      case AnalysisTestCommandType.assertMinCounts:
        _assertMinCounts(
          command.intArg(0),
          command.intArg(1),
          command.intArg(2),
        );
      case AnalysisTestCommandType.assertCodec:
        final expected = _parseCodec(command.stringArg(0));
        if (host.analysisCodec != expected) {
          throw AssertionError(
            'Expected codec ${analysisCodecName(expected)}, got '
            '${analysisCodecName(host.analysisCodec)}',
          );
        }
      case AnalysisTestCommandType.assertNaluName:
        final index = command.intArg(0);
        if (index < 0 || index >= host.analysisNalus.length) {
          throw AssertionError(
            'NALU index $index out of range; '
            'nalus=${host.analysisNalus.length}',
          );
        }
        final expected = command.stringArg(1);
        final actual = bitstreamUnitTypeName(
          host.analysisCodec,
          host.analysisNalus[index].nalType,
        );
        if (actual != expected) {
          throw AssertionError(
            'Expected NALU #$index name $expected, got $actual',
          );
        }
      case AnalysisTestCommandType.assertSelectedFrame:
        _assertSelectedFrame(command.stringArg(0), command.stringArg(1));
      case AnalysisTestCommandType.assertSelectedFrameVisible:
        _assertSelectedFrameVisible();
      case AnalysisTestCommandType.assertReferenceEdges:
        _assertReferenceEdges(command.intArg(0, defaultValue: 1));
      case AnalysisTestCommandType.assertReferenceMaxLayer:
        _assertReferenceMaxLayer(command.intArg(0, defaultValue: 1));
      case AnalysisTestCommandType.setTab:
        host.updateAnalysisTestState(
          () => host.setAnalysisTabForTest(_parseTab(command.stringArg(0))),
        );
      case AnalysisTestCommandType.assertTab:
        final expected = _parseTab(command.stringArg(0));
        if (host.analysisSelectedTab != expected) {
          throw AssertionError(
            'Expected analysis tab $expected, got '
            '${host.analysisSelectedTab}',
          );
        }
      case AnalysisTestCommandType.setOrder:
        host.updateAnalysisTestState(
          () => host.setAnalysisOrderForTest(_parseOrder(command.stringArg(0))),
        );
      case AnalysisTestCommandType.assertOrder:
        final expected = _parseOrder(command.stringArg(0));
        if (host.analysisPtsOrder != expected) {
          throw AssertionError(
            'Expected analysis order ${expected ? 'PTS' : 'DTS'}, got '
            '${host.analysisPtsOrder ? 'PTS' : 'DTS'}',
          );
        }
      case AnalysisTestCommandType.setLayerMode:
        host.updateAnalysisTestState(
          () => host.setAnalysisReferencePyramidLayerModeForTest(
            _parseLayerMode(command.stringArg(0)),
          ),
        );
      case AnalysisTestCommandType.assertLayerMode:
        final expected = _parseLayerMode(command.stringArg(0));
        if (host.analysisReferencePyramidActualTemporalLayers != expected) {
          throw AssertionError(
            'Expected analysis layer mode '
            '${expected ? 'actual' : 'auto'}',
          );
        }
      case AnalysisTestCommandType.setChartWindow:
        host.updateAnalysisTestState(
          () => host.setAnalysisChartWindowForTest(
            command.doubleArg(0),
            command.doubleArg(1),
          ),
        );
      case AnalysisTestCommandType.selectNalu:
        final index = command.intArg(0);
        final total =
            host.analysisSummary?.naluCount ?? host.analysisNalus.length;
        if (index < 0 || index >= total) {
          throw AssertionError('NALU index $index out of range; nalus=$total');
        }
        host.updateAnalysisTestState(
          () => host.selectAnalysisNaluForTest(index),
        );
      case AnalysisTestCommandType.assertDetailVisible:
        final index = host.selectedAnalysisNaluIdx;
        final local = index == null ? -1 : index - host.analysisNaluIndexBase;
        if (index == null || local < 0 || local >= host.analysisNalus.length) {
          throw AssertionError(
            'Expected selected NALU detail, got selected=$index '
            'window=[${host.analysisNaluIndexBase}, '
            '${host.analysisNaluIndexBase + host.analysisNalus.length})',
          );
        }
      case AnalysisTestCommandType.activateFrame:
        final frameIdx = command.intArg(0);
        final total =
            host.analysisSummary?.frameCount ?? host.analysisFrames.length;
        if (frameIdx < 0 || frameIdx >= total) {
          throw AssertionError(
            'Analysis frame $frameIdx out of range; frames=$total',
          );
        }
        if (host.sortedPositionForFrameIdx(frameIdx) == null) {
          throw AssertionError(
            'Analysis frame $frameIdx is not in the loaded chart window',
          );
        }
        host.updateAnalysisTestState(
          () => host.activateAnalysisFrameForTest(frameIdx),
        );
      case AnalysisTestCommandType.assertCurrentFrame:
        final expected = command.intArg(0);
        if (host.currentAnalysisFrameIdx != expected) {
          throw AssertionError(
            'Expected analysis current frame $expected, got '
            '${host.currentAnalysisFrameIdx}',
          );
        }
    }
  }

  Future<void> _waitForLoaded(Duration timeout) async {
    final stopwatch = Stopwatch()..start();
    while (stopwatch.elapsed < timeout) {
      host.readAnalysisDataForTest();
      if (host.isAnalysisLoaded) return;
      await Future<void>.delayed(const Duration(milliseconds: 100));
    }
    throw AssertionError(
      'WAIT_ANALYSIS_LOADED timed out after ${timeout.inMilliseconds}ms',
    );
  }

  void _assertLoaded() {
    if (!host.isAnalysisLoaded) {
      throw AssertionError(
        'Expected analysis loaded; frames=${host.analysisFrames.length}, '
        'packets=${host.analysisSummary?.packetCount ?? 0}, '
        'nalus=${host.analysisNalus.length}',
      );
    }
  }

  void _assertCounts(int frames, int packets, int nalus) {
    _assertLoaded();
    final actualFrames =
        host.analysisSummary?.frameCount ?? host.analysisFrames.length;
    final actualPackets = host.analysisSummary?.packetCount ?? 0;
    final actualNalus =
        host.analysisSummary?.naluCount ?? host.analysisNalus.length;
    if (actualFrames != frames ||
        actualPackets != packets ||
        actualNalus != nalus) {
      throw AssertionError(
        'Expected analysis counts ($frames, $packets, $nalus), got '
        '($actualFrames, $actualPackets, $actualNalus)',
      );
    }
  }

  void _assertMinCounts(int frames, int packets, int nalus) {
    _assertLoaded();
    final actualFrames =
        host.analysisSummary?.frameCount ?? host.analysisFrames.length;
    final actualPackets = host.analysisSummary?.packetCount ?? 0;
    final actualNalus =
        host.analysisSummary?.naluCount ?? host.analysisNalus.length;
    if (actualFrames < frames ||
        actualPackets < packets ||
        actualNalus < nalus) {
      throw AssertionError(
        'Expected analysis counts >= ($frames, $packets, $nalus), got '
        '($actualFrames, $actualPackets, $actualNalus)',
      );
    }
  }

  void _assertSelectedFrame(String expectedSlice, String expectedNalu) {
    final index = host.selectedAnalysisFrameIdx;
    final local = index == null ? -1 : index - host.analysisFrameIndexBase;
    if (index == null || local < 0 || local >= host.analysisFrames.length) {
      throw AssertionError('Expected a selected analysis frame');
    }
    final frame = host.analysisFrames[local];
    final slice = analysisFrameSliceName(frame);
    final nalu = bitstreamUnitTypeName(host.analysisCodec, frame.nalType);
    if (slice != expectedSlice || nalu != expectedNalu) {
      throw AssertionError(
        'Expected selected frame $expectedSlice/$expectedNalu, got '
        '$slice/$nalu',
      );
    }
  }

  void _assertSelectedFrameVisible() {
    final index = host.selectedAnalysisFrameIdx;
    final sorted = index == null ? null : host.sortedPositionForFrameIdx(index);
    if (index == null ||
        sorted == null ||
        sorted < host.analysisChartOffset ||
        sorted >= host.analysisChartOffset + host.analysisVisibleFrameCount) {
      throw AssertionError(
        'Expected selected frame $index (sorted=$sorted) inside chart range',
      );
    }
  }

  void _assertReferenceEdges(int minimum) {
    _assertLoaded();
    final pocCounts = <int, int>{};
    for (final frame in host.analysisFrames) {
      pocCounts[frame.poc] = (pocCounts[frame.poc] ?? 0) + 1;
    }
    var edges = 0;
    for (final frame in host.analysisFrames) {
      for (var i = 0; i < frame.numRefL0 && i < frame.refPocsL0.length; i++) {
        if ((pocCounts[frame.refPocsL0[i]] ?? 0) > 0) edges++;
      }
      for (var i = 0; i < frame.numRefL1 && i < frame.refPocsL1.length; i++) {
        if ((pocCounts[frame.refPocsL1[i]] ?? 0) > 0) edges++;
      }
    }
    if (edges < minimum) {
      throw AssertionError(
        'Expected at least $minimum reference edges, got $edges',
      );
    }
  }

  void _assertReferenceMaxLayer(int minimum) {
    _assertLoaded();
    final pocToIndex = <int, int>{};
    for (var i = 0; i < host.analysisFrames.length; i++) {
      pocToIndex.putIfAbsent(host.analysisFrames[i].poc, () => i);
    }
    final memo = <int, int>{};
    final visiting = <int>{};
    int layerFor(int index) {
      final cached = memo[index];
      if (cached != null) return cached;
      if (!visiting.add(index)) return 0;
      final frame = host.analysisFrames[index];
      var layer = 0;
      if (frame.sliceType == 0) {
        for (var i = 0; i < frame.numRefL0 && i < frame.refPocsL0.length; i++) {
          final reference = pocToIndex[frame.refPocsL0[i]];
          if (reference != null) {
            layer = math.max(layer, layerFor(reference) + 1);
          }
        }
        for (var i = 0; i < frame.numRefL1 && i < frame.refPocsL1.length; i++) {
          final reference = pocToIndex[frame.refPocsL1[i]];
          if (reference != null) {
            layer = math.max(layer, layerFor(reference) + 1);
          }
        }
      }
      visiting.remove(index);
      return memo[index] = layer;
    }

    var maximum = 0;
    for (var i = 0; i < host.analysisFrames.length; i++) {
      maximum = math.max(maximum, layerFor(i));
    }
    if (maximum < minimum) {
      throw AssertionError(
        'Expected reference max layer >= $minimum, got $maximum',
      );
    }
  }
}

int _parseTab(String value) {
  return switch (value.trim().toLowerCase()) {
    '0' || 'ref' || 'reference' || 'ref_pyramid' || 'reference_pyramid' => 0,
    '1' || 'trend' || 'frame_trend' => 1,
    _ => throw ArgumentError('Unknown analysis tab: $value'),
  };
}

bool _parseOrder(String value) {
  return switch (value.trim().toLowerCase()) {
    'pts' => true,
    'dts' || 'decode' || 'decode_order' => false,
    _ => throw ArgumentError('Unknown analysis order: $value'),
  };
}

bool _parseLayerMode(String value) {
  return switch (value.trim().toLowerCase()) {
    'auto' || 'reference' || 'ref' => false,
    'actual' || 'temporal' || 'temporal_id' => true,
    _ => throw ArgumentError('Unknown analysis layer mode: $value'),
  };
}

AnalysisCodec _parseCodec(String value) {
  return switch (value.trim().toLowerCase()) {
    'h264' || 'avc' => AnalysisCodec.h264,
    'h265' || 'hevc' => AnalysisCodec.hevc,
    'h266' || 'vvc' => AnalysisCodec.vvc,
    'av1' => AnalysisCodec.av1,
    'vp9' => AnalysisCodec.vp9,
    'mpeg2' || 'mpeg-2' => AnalysisCodec.mpeg2,
    _ => AnalysisCodec.unknown,
  };
}

import 'dart:async';
import 'dart:io';

import '../../../app_log.dart';
import '../../../analysis/nalu_types.dart';
import '../widgets/analysis_frame_utils.dart';
import 'analysis_test_host.dart';

extension AnalysisPageTestRunner on AnalysisTestHost {
  Future<void> runAnalysisTestScript(String scriptPath) async {
    final instructions = _parseAnalysisTestScript(scriptPath);
    if (instructions.isEmpty) {
      log.severe('AnalysisTestRunner: empty script: $scriptPath');
      exit(1);
    }

    log.info(
      'AnalysisTestRunner: running ${instructions.length} instructions from $scriptPath',
    );

    final sw = Stopwatch()..start();
    for (final instr in instructions) {
      final waitMs = instr.time.inMilliseconds - sw.elapsedMilliseconds;
      if (waitMs > 0) {
        await Future.delayed(Duration(milliseconds: waitMs));
      }

      try {
        await _executeAnalysisInstruction(instr);
      } catch (e, stack) {
        log.severe('AnalysisTestRunner FAIL at ${instr.time}: $e\n$stack');
        exit(1);
      }
    }

    log.severe('AnalysisTestRunner: script ended without QUIT instruction');
    exit(1);
  }

  Future<void> _executeAnalysisInstruction(
    _AnalysisTestInstruction instr,
  ) async {
    switch (instr.command) {
      case _AnalysisTestCommand.waitLoaded:
        final timeout = Duration(
          milliseconds: instr.intArg(0, defaultValue: 10000),
        );
        log.info(
          'AnalysisTestRunner ${instr.time}: WAIT_ANALYSIS_LOADED ${timeout.inMilliseconds}ms',
        );
        await _waitForAnalysisLoaded(timeout);

      case _AnalysisTestCommand.assertLoaded:
        log.info('AnalysisTestRunner ${instr.time}: ASSERT_ANALYSIS_LOADED');
        _assertAnalysisLoaded();

      case _AnalysisTestCommand.assertMinCounts:
        final minFrames = instr.intArg(0);
        final minPackets = instr.intArg(1);
        final minNalus = instr.intArg(2);
        log.info(
          'AnalysisTestRunner ${instr.time}: ASSERT_ANALYSIS_MIN_COUNTS '
          '$minFrames $minPackets $minNalus',
        );
        _assertAnalysisMinCounts(minFrames, minPackets, minNalus);

      case _AnalysisTestCommand.assertCodec:
        final expected = _parseAnalysisCodec(instr.stringArg(0));
        final actual = analysisCodec;
        log.info(
          'AnalysisTestRunner ${instr.time}: ASSERT_ANALYSISanalysisCodec '
          '${analysisCodecName(expected)}',
        );
        if (actual != expected) {
          throw AssertionError(
            'Expected codec ${analysisCodecName(expected)}, '
            'got ${analysisCodecName(actual)}',
          );
        }

      case _AnalysisTestCommand.assertNaluName:
        final idx = instr.intArg(0);
        final expected = instr.stringArg(1);
        log.info(
          'AnalysisTestRunner ${instr.time}: ASSERT_ANALYSIS_NALU_NAME '
          '$idx $expected',
        );
        if (idx < 0 || idx >= analysisNalus.length) {
          throw AssertionError(
            'NALU index $idx out of range; nalus=${analysisNalus.length}',
          );
        }
        final actual = bitstreamUnitTypeName(
          analysisCodec,
          analysisNalus[idx].nalType,
        );
        if (actual != expected) {
          throw AssertionError(
            'Expected NALU #$idx name $expected, got $actual '
            '(codec=${analysisCodecName(analysisCodec)}, '
            'type=${analysisNalus[idx].nalType})',
          );
        }

      case _AnalysisTestCommand.assertSelectedFrame:
        final expectedSlice = instr.stringArg(0);
        final expectedNalName = instr.stringArg(1);
        log.info(
          'AnalysisTestRunner ${instr.time}: '
          'ASSERT_ANALYSIS_SELECTED_FRAME $expectedSlice $expectedNalName',
        );
        final idx = selectedAnalysisFrameIdx;
        final localIdx = idx != null ? idx - analysisFrameIndexBase : -1;
        if (idx == null || localIdx < 0 || localIdx >= analysisFrames.length) {
          throw AssertionError(
            'Expected selected frame, got selected=$idx '
            'window=[$analysisFrameIndexBase, ${analysisFrameIndexBase + analysisFrames.length})',
          );
        }
        final f = analysisFrames[localIdx];
        final actualSlice = analysisFrameSliceName(f);
        final actualNalName = bitstreamUnitTypeName(analysisCodec, f.nalType);
        if (actualSlice != expectedSlice || actualNalName != expectedNalName) {
          throw AssertionError(
            'Expected selected frame $expectedSlice/$expectedNalName, '
            'got $actualSlice/$actualNalName '
            '(frame=$idx, slice=${f.sliceType}, nal=${f.nalType})',
          );
        }

      case _AnalysisTestCommand.assertSelectedFrameVisible:
        log.info(
          'AnalysisTestRunner ${instr.time}: '
          'ASSERT_ANALYSIS_SELECTED_FRAME_VISIBLE',
        );
        final idx = selectedAnalysisFrameIdx;
        final totalFrames =
            analysisSummary?.frameCount ?? analysisFrames.length;
        if (idx == null || idx < 0 || idx >= totalFrames) {
          throw AssertionError(
            'Expected selected frame, got selected=$idx frames=$totalFrames',
          );
        }
        final sortedIdx = sortedPositionForFrameIdx(idx);
        if (sortedIdx == null ||
            sortedIdx < analysisChartOffset ||
            sortedIdx >= analysisChartOffset + analysisVisibleFrameCount) {
          throw AssertionError(
            'Expected selected frame $idx (sorted=$sortedIdx) inside chart '
            'range [$analysisChartOffset, ${analysisChartOffset + analysisVisibleFrameCount})',
          );
        }

      case _AnalysisTestCommand.assertCounts:
        final frames = instr.intArg(0);
        final packets = instr.intArg(1);
        final nalus = instr.intArg(2);
        log.info(
          'AnalysisTestRunner ${instr.time}: ASSERT_ANALYSIS_COUNTS '
          '$frames $packets $nalus',
        );
        _assertAnalysisCounts(frames, packets, nalus);

      case _AnalysisTestCommand.setTab:
        final tab = _parseAnalysisTab(instr.stringArg(0));
        log.info('AnalysisTestRunner ${instr.time}: SET_ANALYSIS_TAB $tab');
        updateAnalysisTestState(() => setAnalysisTabForTest(tab));

      case _AnalysisTestCommand.assertTab:
        final tab = _parseAnalysisTab(instr.stringArg(0));
        log.info('AnalysisTestRunner ${instr.time}: ASSERT_ANALYSIS_TAB $tab');
        if (analysisSelectedTab != tab) {
          throw AssertionError('Expected tab $tab, got $analysisSelectedTab');
        }

      case _AnalysisTestCommand.setOrder:
        final ptsOrder = _parseAnalysisOrder(instr.stringArg(0));
        log.info(
          'AnalysisTestRunner ${instr.time}: SET_ANALYSIS_ORDER '
          '${ptsOrder ? 'PTS' : 'DTS'}',
        );
        updateAnalysisTestState(() {
          setAnalysisOrderForTest(ptsOrder);
        });

      case _AnalysisTestCommand.assertOrder:
        final ptsOrder = _parseAnalysisOrder(instr.stringArg(0));
        log.info(
          'AnalysisTestRunner ${instr.time}: ASSERT_ANALYSIS_ORDER '
          '${ptsOrder ? 'PTS' : 'DTS'}',
        );
        if (analysisPtsOrder != ptsOrder) {
          throw AssertionError(
            'Expected order ${ptsOrder ? 'PTS' : 'DTS'}, '
            'got ${analysisPtsOrder ? 'PTS' : 'DTS'}',
          );
        }

      case _AnalysisTestCommand.selectNalu:
        final idx = instr.intArg(0);
        log.info('AnalysisTestRunner ${instr.time}: SELECT_ANALYSIS_NALU $idx');
        final totalNalus = analysisSummary?.naluCount ?? analysisNalus.length;
        if (idx < 0 || idx >= totalNalus) {
          throw AssertionError(
            'NALU index $idx out of range; nalus=$totalNalus',
          );
        }
        updateAnalysisTestState(() => selectAnalysisNaluForTest(idx));

      case _AnalysisTestCommand.assertDetailVisible:
        log.info(
          'AnalysisTestRunner ${instr.time}: ASSERT_ANALYSIS_DETAIL_VISIBLE',
        );
        final idx = selectedAnalysisNaluIdx;
        final localIdx = idx != null ? idx - analysisNaluIndexBase : -1;
        if (idx == null || localIdx < 0 || localIdx >= analysisNalus.length) {
          throw AssertionError(
            'Expected selected NALU detail, got selected=$idx '
            'window=[$analysisNaluIndexBase, ${analysisNaluIndexBase + analysisNalus.length})',
          );
        }

      case _AnalysisTestCommand.setChartWindow:
        final offset = instr.doubleArg(0);
        final visibleFrameCount = instr.doubleArg(1);
        log.info(
          'AnalysisTestRunner ${instr.time}: SET_ANALYSIS_CHART_WINDOW '
          '$offset $visibleFrameCount',
        );
        updateAnalysisTestState(
          () => setAnalysisChartWindowForTest(offset, visibleFrameCount),
        );

      case _AnalysisTestCommand.quit:
        final exitCode = instr.intArg(0, defaultValue: 0);
        log.info('AnalysisTestRunner ${instr.time}: QUIT $exitCode');
        exit(exitCode);
    }
  }

  Future<void> _waitForAnalysisLoaded(Duration timeout) async {
    final sw = Stopwatch()..start();
    while (sw.elapsed < timeout) {
      readAnalysisDataForTest();
      if (isAnalysisLoaded) return;
      await Future.delayed(const Duration(milliseconds: 100));
    }
    throw AssertionError(
      'WAIT_ANALYSIS_LOADED timed out after ${timeout.inMilliseconds}ms; '
      'loaded=${analysisSummary?.loaded ?? 0}, frames=${analysisFrames.length}, '
      'packets=${analysisSummary?.packetCount ?? 0}, nalus=${analysisNalus.length}',
    );
  }

  void _assertAnalysisLoaded() {
    if (!isAnalysisLoaded) {
      throw AssertionError(
        'Expected analysis loaded; loaded=${analysisSummary?.loaded ?? 0}, '
        'frames=${analysisFrames.length}, packets=${analysisSummary?.packetCount ?? 0}, '
        'nalus=${analysisNalus.length}',
      );
    }
  }

  void _assertAnalysisMinCounts(int minFrames, int minPackets, int minNalus) {
    _assertAnalysisLoaded();
    final actualFrames = analysisSummary?.frameCount ?? analysisFrames.length;
    final packets = analysisSummary?.packetCount ?? 0;
    final actualNalus = analysisSummary?.naluCount ?? analysisNalus.length;
    if (actualFrames < minFrames ||
        packets < minPackets ||
        actualNalus < minNalus) {
      throw AssertionError(
        'Expected analysis counts >= ($minFrames, $minPackets, $minNalus), '
        'got frames=$actualFrames, packets=$packets, nalus=$actualNalus',
      );
    }
  }

  void _assertAnalysisCounts(int frames, int packets, int nalus) {
    _assertAnalysisLoaded();
    final actualFrames = analysisSummary?.frameCount ?? analysisFrames.length;
    final actualPackets = analysisSummary?.packetCount ?? 0;
    final actualNalus = analysisSummary?.naluCount ?? analysisNalus.length;
    if (actualFrames != frames ||
        actualPackets != packets ||
        actualNalus != nalus) {
      throw AssertionError(
        'Expected analysis counts ($frames, $packets, $nalus), '
        'got frames=$actualFrames, packets=$actualPackets, '
        'nalus=$actualNalus',
      );
    }
  }
}

enum _AnalysisTestCommand {
  waitLoaded,
  assertLoaded,
  assertCounts,
  assertMinCounts,
  assertCodec,
  assertNaluName,
  assertSelectedFrame,
  assertSelectedFrameVisible,
  setTab,
  assertTab,
  setOrder,
  assertOrder,
  setChartWindow,
  selectNalu,
  assertDetailVisible,
  quit,
}

class _AnalysisTestInstruction {
  final Duration time;
  final _AnalysisTestCommand command;
  final List<String> args;

  const _AnalysisTestInstruction(this.time, this.command, this.args);

  String stringArg(int index, {String? defaultValue}) {
    if (index < args.length && args[index].isNotEmpty) return args[index];
    if (defaultValue != null) return defaultValue;
    throw ArgumentError('Missing argument $index for $command');
  }

  int intArg(int index, {int? defaultValue}) {
    if (index < args.length && args[index].isNotEmpty) {
      return int.parse(args[index]);
    }
    if (defaultValue != null) return defaultValue;
    throw ArgumentError('Missing integer argument $index for $command');
  }

  double doubleArg(int index, {double? defaultValue}) {
    if (index < args.length && args[index].isNotEmpty) {
      return double.parse(args[index]);
    }
    if (defaultValue != null) return defaultValue;
    throw ArgumentError('Missing number argument $index for $command');
  }
}

List<_AnalysisTestInstruction> _parseAnalysisTestScript(String path) {
  final file = File(path);
  if (!file.existsSync()) {
    log.severe('Analysis test script not found: $path');
    return [];
  }

  final instructions = <_AnalysisTestInstruction>[];
  final lines = file.readAsLinesSync();

  for (var i = 0; i < lines.length; i++) {
    final line = lines[i].trim();
    if (line.isEmpty || line.startsWith('#') || line.startsWith('@')) continue;

    final parts = line.split(',').map((s) => s.trim()).toList();
    if (parts.length < 2) {
      log.warning('Analysis test line ${i + 1}: invalid format: $line');
      continue;
    }

    final time = Duration(
      milliseconds: (double.parse(parts[0]) * 1000).round(),
    );
    final cmd = parts[1].toUpperCase();
    final args = parts.sublist(2);
    final command = switch (cmd) {
      'WAIT_ANALYSIS_LOADED' => _AnalysisTestCommand.waitLoaded,
      'ASSERT_ANALYSIS_LOADED' => _AnalysisTestCommand.assertLoaded,
      'ASSERT_ANALYSIS_COUNTS' => _AnalysisTestCommand.assertCounts,
      'ASSERT_ANALYSIS_MIN_COUNTS' => _AnalysisTestCommand.assertMinCounts,
      'ASSERT_ANALYSIS_CODEC' => _AnalysisTestCommand.assertCodec,
      'ASSERT_ANALYSISanalysisCodec' => _AnalysisTestCommand.assertCodec,
      'ASSERT_ANALYSIS_NALU_NAME' => _AnalysisTestCommand.assertNaluName,
      'ASSERT_ANALYSIS_SELECTED_FRAME' =>
        _AnalysisTestCommand.assertSelectedFrame,
      'ASSERT_ANALYSIS_SELECTED_FRAME_VISIBLE' =>
        _AnalysisTestCommand.assertSelectedFrameVisible,
      'SET_ANALYSIS_TAB' => _AnalysisTestCommand.setTab,
      'ASSERT_ANALYSIS_TAB' => _AnalysisTestCommand.assertTab,
      'SET_ANALYSIS_ORDER' => _AnalysisTestCommand.setOrder,
      'ASSERT_ANALYSIS_ORDER' => _AnalysisTestCommand.assertOrder,
      'SET_ANALYSIS_CHART_WINDOW' => _AnalysisTestCommand.setChartWindow,
      'SELECT_ANALYSIS_NALU' => _AnalysisTestCommand.selectNalu,
      'ASSERT_ANALYSIS_DETAIL_VISIBLE' =>
        _AnalysisTestCommand.assertDetailVisible,
      'QUIT' => _AnalysisTestCommand.quit,
      _ => null,
    };

    if (command == null) {
      log.warning('Unknown analysis test command: $cmd');
      continue;
    }
    instructions.add(_AnalysisTestInstruction(time, command, args));
  }

  instructions.sort((a, b) => a.time.compareTo(b.time));
  return instructions;
}

int _parseAnalysisTab(String value) {
  switch (value.trim().toLowerCase()) {
    case '0':
    case 'ref':
    case 'reference':
    case 'ref_pyramid':
    case 'reference_pyramid':
      return 0;
    case '1':
    case 'trend':
    case 'frame_trend':
      return 1;
    default:
      throw ArgumentError('Unknown analysis tab: $value');
  }
}

bool _parseAnalysisOrder(String value) {
  switch (value.trim().toLowerCase()) {
    case 'pts':
      return true;
    case 'dts':
    case 'decode':
    case 'decode_order':
      return false;
    default:
      throw ArgumentError('Unknown analysis order: $value');
  }
}

AnalysisCodec _parseAnalysisCodec(String value) {
  switch (value.trim().toLowerCase()) {
    case 'h264':
    case 'avc':
      return AnalysisCodec.h264;
    case 'h265':
    case 'hevc':
      return AnalysisCodec.hevc;
    case 'h266':
    case 'vvc':
      return AnalysisCodec.vvc;
    case 'av1':
      return AnalysisCodec.av1;
    case 'vp9':
      return AnalysisCodec.vp9;
    case 'mpeg2':
    case 'mpeg-2':
      return AnalysisCodec.mpeg2;
    default:
      return AnalysisCodec.unknown;
  }
}

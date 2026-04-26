import 'dart:async';
import 'dart:io';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import '../app_log.dart';
import '../analysis/analysis_cache.dart';
import '../analysis/analysis_ffi.dart';
import '../analysis/nalu_types.dart';
import '../l10n/app_localizations.dart';
import '../widgets/segmented_widget.dart';
import 'analysis_ipc.dart';

// ===========================================================================
// Analysis Window — secondary Flutter window for bitstream visualization
// ===========================================================================

const double _analysisHeaderHeight = 40;
const double _analysisHeaderControlHeight = 32;
const double _analysisHeaderGap = 4;
const EdgeInsets _analysisHeaderPadding = EdgeInsets.all(4);

class AnalysisApp extends StatelessWidget {
  final Color accentColor;
  final String hash;
  final String? fileName;
  final String? testScriptPath;

  const AnalysisApp({
    super.key,
    required this.accentColor,
    required this.hash,
    this.fileName,
    this.testScriptPath,
  });

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: fileName != null
          ? 'Void Player - $fileName'
          : 'Void Player - Analysis',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        brightness: Brightness.dark,
        colorSchemeSeed: accentColor,
        useMaterial3: true,
      ),
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: AnalysisPage(hash: hash, testScriptPath: testScriptPath),
    );
  }
}

class AnalysisWorkspaceApp extends StatelessWidget {
  final Color accentColor;
  final List<String> hashes;
  final List<String?> fileNames;
  final String? testScriptPath;
  final AnalysisIpcClient? ipcClient;

  const AnalysisWorkspaceApp({
    super.key,
    required this.accentColor,
    required this.hashes,
    required this.fileNames,
    this.testScriptPath,
    this.ipcClient,
  });

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Void Player - Analysis',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        brightness: Brightness.dark,
        colorSchemeSeed: accentColor,
        useMaterial3: true,
      ),
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: _AnalysisWorkspacePage(
        entries: [
          for (var i = 0; i < hashes.length; i++)
            _AnalysisWorkspaceEntry(
              hash: hashes[i],
              fileName: i < fileNames.length ? fileNames[i] : null,
            ),
        ],
        testScriptPath: testScriptPath,
        ipcClient: ipcClient,
      ),
    );
  }
}

class _AnalysisWorkspaceEntry {
  final String hash;
  final String? fileName;

  const _AnalysisWorkspaceEntry({required this.hash, this.fileName});

  factory _AnalysisWorkspaceEntry.fromIpcTrack(AnalysisIpcTrack track) =>
      _AnalysisWorkspaceEntry(hash: track.hash, fileName: track.fileName);
}

class _AnalysisWorkspacePage extends StatefulWidget {
  final List<_AnalysisWorkspaceEntry> entries;
  final String? testScriptPath;
  final AnalysisIpcClient? ipcClient;

  const _AnalysisWorkspacePage({
    required this.entries,
    this.testScriptPath,
    this.ipcClient,
  });

  @override
  State<_AnalysisWorkspacePage> createState() => _AnalysisWorkspacePageState();
}

class _AnalysisWorkspacePageState extends State<_AnalysisWorkspacePage> {
  int _selected = 0;
  bool _splitView = false;
  late List<_AnalysisWorkspaceEntry> _entries;

  @override
  void initState() {
    super.initState();
    _entries = widget.entries;
    widget.ipcClient?.addListener(_onIpcTracksChanged);
    _onIpcTracksChanged();
  }

  @override
  void dispose() {
    widget.ipcClient?.removeListener(_onIpcTracksChanged);
    widget.ipcClient?.dispose();
    super.dispose();
  }

  void _onIpcTracksChanged() {
    final client = widget.ipcClient;
    if (client == null || !client.hasSnapshot) return;
    final selectedHash = _entries.isNotEmpty
        ? _entries[_selected.clamp(0, _entries.length - 1)].hash
        : null;
    final entries = [
      for (final track in client.tracks)
        if (track.hash.isNotEmpty) _AnalysisWorkspaceEntry.fromIpcTrack(track),
    ];
    final nextSelected = selectedHash == null
        ? entries.isEmpty
              ? 0
              : _selected.clamp(0, entries.length - 1)
        : entries.indexWhere((entry) => entry.hash == selectedHash);

    void applySnapshot() {
      _entries = entries;
      if (entries.length <= 1) {
        _splitView = false;
      }
      _selected = entries.isEmpty
          ? 0
          : nextSelected >= 0
          ? nextSelected
          : _selected.clamp(0, entries.length - 1);
    }

    if (!mounted) {
      applySnapshot();
      return;
    }
    setState(applySnapshot);
  }

  @override
  Widget build(BuildContext context) {
    final entries = _entries;
    if (entries.isEmpty) {
      return const Scaffold(body: SizedBox.shrink());
    }
    final selected = _selected.clamp(0, entries.length - 1);
    final modeToggleEnabled = entries.length > 1;

    return Scaffold(
      body: _splitView
          ? _AnalysisSplitView(
              entries: entries,
              splitView: _splitView,
              modeToggleEnabled: modeToggleEnabled,
              selectedIndex: selected,
              onModeChanged: (value) => setState(() => _splitView = value),
              onSelected: (index) => setState(() => _selected = index),
            )
          : _AnalysisTabbedView(
              entries: entries,
              selectedIndex: selected,
              splitView: _splitView,
              modeToggleEnabled: modeToggleEnabled,
              onModeChanged: (value) => setState(() => _splitView = value),
              onSelected: (index) => setState(() => _selected = index),
              child: AnalysisPage(
                key: ValueKey('analysis-${entries[selected].hash}'),
                hash: entries[selected].hash,
                testScriptPath: selected == 0 ? widget.testScriptPath : null,
                pollSummary: false,
              ),
            ),
    );
  }
}

class _AnalysisTabbedView extends StatelessWidget {
  final List<_AnalysisWorkspaceEntry> entries;
  final int selectedIndex;
  final bool splitView;
  final bool modeToggleEnabled;
  final ValueChanged<bool> onModeChanged;
  final ValueChanged<int> onSelected;
  final Widget child;

  const _AnalysisTabbedView({
    required this.entries,
    required this.selectedIndex,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.onModeChanged,
    required this.onSelected,
    required this.child,
  });

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        _AnalysisTabsHeader(
          entries: entries,
          selectedIndex: selectedIndex,
          splitView: splitView,
          modeToggleEnabled: modeToggleEnabled,
          onModeChanged: onModeChanged,
          onSelected: onSelected,
        ),
        Expanded(child: child),
      ],
    );
  }
}

class _AnalysisTabsHeader extends StatelessWidget {
  final List<_AnalysisWorkspaceEntry> entries;
  final int selectedIndex;
  final bool splitView;
  final bool modeToggleEnabled;
  final ValueChanged<bool> onModeChanged;
  final ValueChanged<int> onSelected;

  const _AnalysisTabsHeader({
    required this.entries,
    required this.selectedIndex,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.onModeChanged,
    required this.onSelected,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Container(
      height: _analysisHeaderHeight,
      padding: _analysisHeaderPadding,
      decoration: BoxDecoration(
        color: theme.colorScheme.primaryContainer.withValues(alpha: 0.18),
        border: Border(bottom: BorderSide(color: theme.dividerColor)),
      ),
      child: Row(
        children: [
          _AnalysisWorkspaceModeToggle(
            splitView: splitView,
            enabled: modeToggleEnabled,
            onChanged: onModeChanged,
          ),
          const SizedBox(width: _analysisHeaderGap),
          Expanded(
            child: Row(
              children: [
                for (var i = 0; i < entries.length; i++)
                  Expanded(
                    child: _AnalysisTrackTab(
                      entry: entries[i],
                      index: i,
                      selected: i == selectedIndex,
                      onTap: () => onSelected(i),
                    ),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _AnalysisTrackTab extends StatelessWidget {
  final _AnalysisWorkspaceEntry entry;
  final int index;
  final bool selected;
  final VoidCallback onTap;

  const _AnalysisTrackTab({
    required this.entry,
    required this.index,
    required this.selected,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 2),
      child: _AnalysisTrackTitleButton(
        entry: entry,
        index: index,
        selected: selected,
        onTap: selected ? null : onTap,
      ),
    );
  }
}

class _AnalysisTrackTitleButton extends StatelessWidget {
  final _AnalysisWorkspaceEntry entry;
  final int index;
  final bool selected;
  final VoidCallback? onTap;

  const _AnalysisTrackTitleButton({
    required this.entry,
    required this.index,
    required this.selected,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return SizedBox(
      height: _analysisHeaderControlHeight,
      child: Material(
        color: selected
            ? theme.colorScheme.primary.withValues(alpha: 0.10)
            : Colors.transparent,
        borderRadius: BorderRadius.circular(4),
        clipBehavior: Clip.antiAlias,
        child: InkWell(
          borderRadius: BorderRadius.circular(4),
          onTap: onTap,
          child: Stack(
            children: [
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 10),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: Text(
                    '${index + 1}. ${entry.fileName ?? 'Track ${index + 1}'}',
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    softWrap: false,
                    style: theme.textTheme.titleSmall?.copyWith(
                      color: selected
                          ? theme.colorScheme.onSurface
                          : theme.colorScheme.onSurfaceVariant,
                      fontWeight: selected ? FontWeight.w600 : null,
                    ),
                  ),
                ),
              ),
              if (selected)
                Positioned(
                  left: 10,
                  right: 10,
                  bottom: 2,
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      color: theme.colorScheme.primary.withValues(alpha: 0.52),
                      borderRadius: BorderRadius.circular(1),
                    ),
                    child: const SizedBox(height: 2),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class _AnalysisSplitView extends StatelessWidget {
  final List<_AnalysisWorkspaceEntry> entries;
  final bool splitView;
  final bool modeToggleEnabled;
  final int selectedIndex;
  final ValueChanged<bool> onModeChanged;
  final ValueChanged<int> onSelected;

  const _AnalysisSplitView({
    required this.entries,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.selectedIndex,
    required this.onModeChanged,
    required this.onSelected,
  });

  @override
  Widget build(BuildContext context) {
    final count = entries.length;
    final columns = count <= 2 ? count : 2;
    final rows = (count / columns).ceil();

    return Column(
      children: [
        for (var row = 0; row < rows; row++)
          Expanded(
            child: Row(
              children: [
                for (var col = 0; col < columns; col++)
                  Expanded(
                    child: _splitCell(
                      context,
                      row * columns + col,
                      row: row,
                      col: col,
                      rows: rows,
                      columns: columns,
                    ),
                  ),
              ],
            ),
          ),
      ],
    );
  }

  Widget _splitCell(
    BuildContext context,
    int index, {
    required int row,
    required int col,
    required int rows,
    required int columns,
  }) {
    if (index >= entries.length) return const SizedBox.shrink();
    final entry = entries[index];
    final theme = Theme.of(context);
    final divider = BorderSide(color: theme.colorScheme.outlineVariant);
    return Container(
      decoration: BoxDecoration(
        border: Border(
          right: col < columns - 1 ? divider : BorderSide.none,
          bottom: row < rows - 1 ? divider : BorderSide.none,
        ),
      ),
      child: _AnalysisTrackPane(
        entry: entry,
        index: index,
        selected: index == selectedIndex,
        showModeToggle: index == 0,
        splitView: splitView,
        modeToggleEnabled: modeToggleEnabled,
        onModeChanged: onModeChanged,
        onSelected: () => onSelected(index),
        child: AnalysisPage(
          key: ValueKey('analysis-split-${entry.hash}'),
          hash: entry.hash,
          pollSummary: false,
        ),
      ),
    );
  }
}

class _AnalysisTrackPane extends StatelessWidget {
  final _AnalysisWorkspaceEntry entry;
  final int index;
  final bool selected;
  final bool showModeToggle;
  final bool splitView;
  final bool modeToggleEnabled;
  final ValueChanged<bool> onModeChanged;
  final VoidCallback? onSelected;
  final Widget child;

  const _AnalysisTrackPane({
    required this.entry,
    required this.index,
    required this.selected,
    required this.showModeToggle,
    required this.splitView,
    required this.modeToggleEnabled,
    required this.onModeChanged,
    required this.child,
    this.onSelected,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Column(
      children: [
        Container(
          height: _analysisHeaderHeight,
          padding: _analysisHeaderPadding,
          decoration: BoxDecoration(
            color: selected
                ? theme.colorScheme.primaryContainer.withValues(alpha: 0.18)
                : theme.colorScheme.surface.withValues(alpha: 0.18),
            border: Border(bottom: BorderSide(color: theme.dividerColor)),
          ),
          child: Row(
            children: [
              if (showModeToggle) ...[
                _AnalysisWorkspaceModeToggle(
                  splitView: splitView,
                  enabled: modeToggleEnabled,
                  onChanged: onModeChanged,
                ),
                const SizedBox(width: _analysisHeaderGap),
              ],
              Expanded(
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 2),
                  child: _AnalysisTrackTitleButton(
                    entry: entry,
                    index: index,
                    selected: selected,
                    onTap: onSelected,
                  ),
                ),
              ),
            ],
          ),
        ),
        Expanded(child: child),
      ],
    );
  }
}

class _AnalysisWorkspaceModeToggle extends StatelessWidget {
  final bool splitView;
  final bool enabled;
  final ValueChanged<bool> onChanged;

  const _AnalysisWorkspaceModeToggle({
    required this.splitView,
    required this.enabled,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return ExcludeSemantics(
      child: ViewModeSelector(
        currentMode: splitView ? 1 : 0,
        onChanged: (value) => onChanged(value == 1),
        firstLabel: l.analysisTabsMode,
        secondLabel: l.analysisSplitMode,
        width: 124,
        height: _analysisHeaderControlHeight,
        enabled: enabled,
        labelFontWeight: FontWeight.w700,
      ),
    );
  }
}

// ===========================================================================

class AnalysisPage extends StatefulWidget {
  final String hash;
  final String? testScriptPath;
  final bool pollSummary;
  const AnalysisPage({
    super.key,
    required this.hash,
    this.testScriptPath,
    this.pollSummary = true,
  });

  @override
  State<AnalysisPage> createState() => _AnalysisPageState();
}

class _AnalysisPageState extends State<AnalysisPage> {
  int _selectedTab = 0; // 0=ref pyramid, 1=frame trend
  bool _ptsOrder = true;
  int? _selectedNaluIdx;
  String _naluFilter = '';
  double _naluBrowserWidth = 300; // draggable splitter width for NALU browser
  int? _selectedFrameIdx;

  // Zoom / scroll state for top chart panel
  double _visibleFrameCount = 10;
  double _chartOffset = 0.0;
  double _frameSizeAxisZoom = 1.0;
  double _qpAxisZoom = 1.0;
  double _topPanelFraction = 0.40;

  void _chartZoom(double scrollDelta) {
    setState(() {
      final factor = scrollDelta > 0 ? 1.18 : 0.85;
      final oldCount = _visibleFrameCount;
      _visibleFrameCount = (_visibleFrameCount * factor).clamp(
        3.0,
        _sortedFrames.length.toDouble(),
      );
      final center = _chartOffset + oldCount / 2;
      _chartOffset = center - _visibleFrameCount / 2;
      _clampChartOffset();
    });
  }

  void _chartPan(double newOffset) {
    setState(() {
      _chartOffset = newOffset;
      _clampChartOffset();
    });
  }

  void _frameTrendAxisZoom(_FrameTrendAxis axis, double scrollDelta) {
    setState(() {
      final factor = scrollDelta > 0 ? 0.85 : 1.18;
      switch (axis) {
        case _FrameTrendAxis.frameSize:
          _frameSizeAxisZoom = (_frameSizeAxisZoom * factor).clamp(0.25, 12.0);
        case _FrameTrendAxis.qp:
          _qpAxisZoom = (_qpAxisZoom * factor).clamp(0.5, 8.0);
      }
    });
  }

  void _clampChartOffset() {
    final max = (_sortedFrames.length - _visibleFrameCount).clamp(
      0.0,
      double.infinity,
    );
    _chartOffset = _chartOffset.clamp(0.0, max);
  }

  List<FrameInfo> _frames = [];
  List<NaluInfo> _nalus = [];
  List<FrameInfo> _sortedFramesCache = [];
  List<int> _sortedFrameOriginalIndicesCache = [];
  List<int> _frameToSortedPosition = [];
  Map<int, List<int>> _sortedPocToIndices = {};
  NakiAnalysisSummary? _summary;
  Timer? _pollTimer;
  bool _testStarted = false;

  // Precomputed mappings rebuilt when data loads
  List<int> _frameToNalu = []; // frameIdx → naluIdx
  List<int?> _naluToFrame = []; // naluIdx → frameIdx (null if non-VCL)

  @override
  void initState() {
    super.initState();
    _loadData();
    if (widget.pollSummary) {
      _pollTimer = Timer.periodic(
        const Duration(milliseconds: 200),
        (_) => _poll(),
      );
    }
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final scriptPath = widget.testScriptPath;
      if (scriptPath != null && !_testStarted) {
        _testStarted = true;
        unawaited(_runAnalysisTestScript(scriptPath));
      }
    });
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    super.dispose();
  }

  void _loadData() {
    final hash = widget.hash;
    final vbs2 = AnalysisCache.vbs2Path(hash);
    final vbi = AnalysisCache.vbiPath(hash);
    final vbt = AnalysisCache.vbtPath(hash);

    AnalysisFfi.load(vbs2, vbi, vbt);
    _readData();
  }

  Future<void> _runAnalysisTestScript(String scriptPath) async {
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
        final actual = _codec;
        log.info(
          'AnalysisTestRunner ${instr.time}: ASSERT_ANALYSIS_CODEC '
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
        if (idx < 0 || idx >= _nalus.length) {
          throw AssertionError(
            'NALU index $idx out of range; nalus=${_nalus.length}',
          );
        }
        final actual = bitstreamUnitTypeName(_codec, _nalus[idx].nalType);
        if (actual != expected) {
          throw AssertionError(
            'Expected NALU #$idx name $expected, got $actual '
            '(codec=${analysisCodecName(_codec)}, '
            'type=${_nalus[idx].nalType})',
          );
        }

      case _AnalysisTestCommand.assertSelectedFrame:
        final expectedSlice = instr.stringArg(0);
        final expectedNalName = instr.stringArg(1);
        log.info(
          'AnalysisTestRunner ${instr.time}: '
          'ASSERT_ANALYSIS_SELECTED_FRAME $expectedSlice $expectedNalName',
        );
        final idx = _selectedFrameIdx;
        if (idx == null || idx < 0 || idx >= _frames.length) {
          throw AssertionError(
            'Expected selected frame, got selected=$idx frames=${_frames.length}',
          );
        }
        final f = _frames[idx];
        final actualSlice = _frameSliceName(f);
        final actualNalName = bitstreamUnitTypeName(_codec, f.nalType);
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
        final idx = _selectedFrameIdx;
        if (idx == null || idx < 0 || idx >= _frames.length) {
          throw AssertionError(
            'Expected selected frame, got selected=$idx frames=${_frames.length}',
          );
        }
        final sortedIdx = _sortedPositionForFrameIdx(idx);
        if (sortedIdx == null ||
            sortedIdx < _chartOffset ||
            sortedIdx >= _chartOffset + _visibleFrameCount) {
          throw AssertionError(
            'Expected selected frame $idx (sorted=$sortedIdx) inside chart '
            'range [$_chartOffset, ${_chartOffset + _visibleFrameCount})',
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
        if (mounted) {
          setState(() => _selectedTab = tab);
        } else {
          _selectedTab = tab;
        }

      case _AnalysisTestCommand.assertTab:
        final tab = _parseAnalysisTab(instr.stringArg(0));
        log.info('AnalysisTestRunner ${instr.time}: ASSERT_ANALYSIS_TAB $tab');
        if (_selectedTab != tab) {
          throw AssertionError('Expected tab $tab, got $_selectedTab');
        }

      case _AnalysisTestCommand.setOrder:
        final ptsOrder = _parseAnalysisOrder(instr.stringArg(0));
        log.info(
          'AnalysisTestRunner ${instr.time}: SET_ANALYSIS_ORDER '
          '${ptsOrder ? 'PTS' : 'DTS'}',
        );
        if (mounted) {
          setState(() {
            _ptsOrder = ptsOrder;
            _rebuildSortedFramesCache();
          });
        } else {
          _ptsOrder = ptsOrder;
          _rebuildSortedFramesCache();
        }

      case _AnalysisTestCommand.assertOrder:
        final ptsOrder = _parseAnalysisOrder(instr.stringArg(0));
        log.info(
          'AnalysisTestRunner ${instr.time}: ASSERT_ANALYSIS_ORDER '
          '${ptsOrder ? 'PTS' : 'DTS'}',
        );
        if (_ptsOrder != ptsOrder) {
          throw AssertionError(
            'Expected order ${ptsOrder ? 'PTS' : 'DTS'}, '
            'got ${_ptsOrder ? 'PTS' : 'DTS'}',
          );
        }

      case _AnalysisTestCommand.selectNalu:
        final idx = instr.intArg(0);
        log.info('AnalysisTestRunner ${instr.time}: SELECT_ANALYSIS_NALU $idx');
        if (idx < 0 || idx >= _nalus.length) {
          throw AssertionError(
            'NALU index $idx out of range; nalus=${_nalus.length}',
          );
        }
        if (mounted) {
          setState(() {
            _selectNalu(idx);
          });
        } else {
          _selectNalu(idx);
        }

      case _AnalysisTestCommand.assertDetailVisible:
        log.info(
          'AnalysisTestRunner ${instr.time}: ASSERT_ANALYSIS_DETAIL_VISIBLE',
        );
        final idx = _selectedNaluIdx;
        if (idx == null || idx < 0 || idx >= _nalus.length) {
          throw AssertionError(
            'Expected selected NALU detail, got selected=$idx '
            'nalus=${_nalus.length}',
          );
        }

      case _AnalysisTestCommand.quit:
        final exitCode = instr.intArg(0, defaultValue: 0);
        log.info('AnalysisTestRunner ${instr.time}: QUIT $exitCode');
        exit(exitCode);
    }
  }

  Future<void> _waitForAnalysisLoaded(Duration timeout) async {
    final sw = Stopwatch()..start();
    while (sw.elapsed < timeout) {
      _readData();
      if (_isAnalysisLoaded) return;
      await Future.delayed(const Duration(milliseconds: 100));
    }
    throw AssertionError(
      'WAIT_ANALYSIS_LOADED timed out after ${timeout.inMilliseconds}ms; '
      'loaded=${_summary?.loaded ?? 0}, frames=${_frames.length}, '
      'packets=${_summary?.packetCount ?? 0}, nalus=${_nalus.length}',
    );
  }

  bool get _isAnalysisLoaded =>
      (_summary?.loaded ?? 0) != 0 && (_frames.isNotEmpty || _nalus.isNotEmpty);

  void _assertAnalysisLoaded() {
    if (!_isAnalysisLoaded) {
      throw AssertionError(
        'Expected analysis loaded; loaded=${_summary?.loaded ?? 0}, '
        'frames=${_frames.length}, packets=${_summary?.packetCount ?? 0}, '
        'nalus=${_nalus.length}',
      );
    }
  }

  void _assertAnalysisMinCounts(int minFrames, int minPackets, int minNalus) {
    _assertAnalysisLoaded();
    final packets = _summary?.packetCount ?? 0;
    if (_frames.length < minFrames ||
        packets < minPackets ||
        _nalus.length < minNalus) {
      throw AssertionError(
        'Expected analysis counts >= ($minFrames, $minPackets, $minNalus), '
        'got frames=${_frames.length}, packets=$packets, nalus=${_nalus.length}',
      );
    }
  }

  void _assertAnalysisCounts(int frames, int packets, int nalus) {
    _assertAnalysisLoaded();
    final actualPackets = _summary?.packetCount ?? 0;
    if (_frames.length != frames ||
        actualPackets != packets ||
        _nalus.length != nalus) {
      throw AssertionError(
        'Expected analysis counts ($frames, $packets, $nalus), '
        'got frames=${_frames.length}, packets=$actualPackets, '
        'nalus=${_nalus.length}',
      );
    }
  }

  void _readData() {
    final s = AnalysisFfi.summary;
    if (s.loaded == 0) return;
    _summary = s;
    _frames = AnalysisFfi.frames;
    _nalus = AnalysisFfi.nalus;
    _rebuildDerivedState();
    if (mounted) {
      setState(() {});
    }
  }

  void _rebuildDerivedState() {
    // Frame ↔ NALU mappings (single pass)
    _frameToNalu = List<int>.filled(_frames.length, -1);
    _naluToFrame = List<int?>.filled(_nalus.length, null);
    var vclIdx = 0;
    for (var i = 0; i < _nalus.length; i++) {
      if ((_nalus[i].flags & 0x01) != 0) {
        if (vclIdx < _frames.length) {
          _frameToNalu[vclIdx] = i;
          _naluToFrame[i] = vclIdx;
        }
        vclIdx++;
      }
    }

    _rebuildSortedFramesCache();
  }

  void _poll() {
    final s = AnalysisFfi.summary;
    if (s.loaded == 0) return;
    if (_summary != null &&
        s.currentFrameIdx == _summary!.currentFrameIdx &&
        s.frameCount == _summary!.frameCount) {
      return;
    }
    _summary = s;
    if (mounted) {
      setState(() {});
    }
  }

  int? _frameToNaluIdx(int frameIdx) {
    if (frameIdx < 0 || frameIdx >= _frameToNalu.length) return null;
    final v = _frameToNalu[frameIdx];
    return v >= 0 ? v : null;
  }

  int? _naluToFrameIdx(int naluIdx) {
    if (naluIdx < 0 || naluIdx >= _naluToFrame.length) return null;
    return _naluToFrame[naluIdx];
  }

  List<FrameInfo> get _sortedFrames => _sortedFramesCache;
  int? get _selectedSortedFrameIdx => _selectedFrameIdx == null
      ? null
      : _sortedPositionForFrameIdx(_selectedFrameIdx!);
  int get _currentSortedFrameIdx {
    final idx = _summary?.currentFrameIdx ?? -1;
    return _sortedPositionForFrameIdx(idx) ?? -1;
  }

  AnalysisCodec get _codec => analysisCodecFromValue(_summary?.codec ?? 0);

  void _rebuildSortedFramesCache() {
    final order = List<int>.generate(_frames.length, (i) => i);
    if (_ptsOrder) {
      order.sort((a, b) => _frames[a].pts.compareTo(_frames[b].pts));
    }
    // else: keep original order (= DTS / decode order from C++)
    _sortedFrameOriginalIndicesCache = order;
    _sortedFramesCache = [for (final idx in order) _frames[idx]];
    _frameToSortedPosition = List<int>.filled(_frames.length, -1);
    _sortedPocToIndices = <int, List<int>>{};
    for (var sortedIdx = 0; sortedIdx < order.length; sortedIdx++) {
      final originalIdx = order[sortedIdx];
      _frameToSortedPosition[originalIdx] = sortedIdx;
      (_sortedPocToIndices[_frames[originalIdx].poc] ??= []).add(sortedIdx);
    }
    _clampChartOffset();
  }

  int? _sortedPositionForFrameIdx(int frameIdx) {
    if (frameIdx < 0 || frameIdx >= _frameToSortedPosition.length) return null;
    final sortedIdx = _frameToSortedPosition[frameIdx];
    return sortedIdx >= 0 ? sortedIdx : null;
  }

  int? _originalFrameIdxAtSortedPosition(int sortedIdx) {
    if (sortedIdx < 0 || sortedIdx >= _sortedFrameOriginalIndicesCache.length) {
      return null;
    }
    return _sortedFrameOriginalIndicesCache[sortedIdx];
  }

  void _centerChartOnFrame(int frameIdx) {
    final sortedIdx = _sortedPositionForFrameIdx(frameIdx);
    if (sortedIdx == null) return;
    _chartOffset = sortedIdx - _visibleFrameCount / 2 + 0.5;
    _clampChartOffset();
  }

  void _centerChartOnSelectedFrame() {
    final idx = _selectedFrameIdx;
    if (idx != null) _centerChartOnFrame(idx);
  }

  void _selectFrame(int? frameIdx, {bool centerChart = false}) {
    _selectedFrameIdx = frameIdx;
    _selectedNaluIdx = frameIdx != null ? _frameToNaluIdx(frameIdx) : null;
    if (centerChart && frameIdx != null) _centerChartOnFrame(frameIdx);
  }

  void _selectChartFrame(int? sortedIdx) {
    _selectFrame(
      sortedIdx != null ? _originalFrameIdxAtSortedPosition(sortedIdx) : null,
    );
  }

  void _selectNalu(int? naluIdx) {
    _selectedNaluIdx = naluIdx;
    final frameIdx = naluIdx != null ? _naluToFrameIdx(naluIdx) : null;
    _selectedFrameIdx = frameIdx;
    if (frameIdx != null) _centerChartOnFrame(frameIdx);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    final topChart = _selectedTab == 0
        ? _ReferencePyramidView(
            frames: _sortedFrames,
            currentIdx: _currentSortedFrameIdx,
            selectedFrameIdx: _selectedSortedFrameIdx,
            pocToIndices: _sortedPocToIndices,
            onFrameSelected: (i) => setState(() => _selectChartFrame(i)),
            viewStart: _chartOffset,
            viewEnd: _chartOffset + _visibleFrameCount,
            ptsOrder: _ptsOrder,
            onZoom: _chartZoom,
            onPan: _chartPan,
            l: l,
          )
        : _FrameTrendView(
            frames: _sortedFrames,
            currentIdx: _currentSortedFrameIdx,
            selectedFrameIdx: _selectedSortedFrameIdx,
            viewStart: _chartOffset,
            viewEnd: _chartOffset + _visibleFrameCount,
            frameSizeAxisZoom: _frameSizeAxisZoom,
            qpAxisZoom: _qpAxisZoom,
            ptsOrder: _ptsOrder,
            onZoom: _chartZoom,
            onAxisZoom: _frameTrendAxisZoom,
            onPan: _chartPan,
            onFrameSelected: (i) => setState(() => _selectChartFrame(i)),
            l: l,
          );
    final bottomPanel = LayoutBuilder(
      builder: (context, constraints) {
        final totalW = constraints.maxWidth;
        final maxBrowserW = totalW - 120; // leave room for detail
        final browserW = _naluBrowserWidth.clamp(120.0, maxBrowserW);
        return Stack(
          children: [
            Row(
              children: [
                SizedBox(
                  width: browserW,
                  child: _NaluBrowserView(
                    nalus: _nalus,
                    codec: _codec,
                    selectedIdx: _selectedNaluIdx,
                    onSelected: (i) => setState(() => _selectNalu(i)),
                    filter: _naluFilter,
                    onFilterChanged: (v) => setState(() => _naluFilter = v),
                  ),
                ),
                Container(width: 1, color: theme.colorScheme.outlineVariant),
                Expanded(
                  child: _NaluDetailView(
                    nalu:
                        _selectedNaluIdx != null &&
                            _selectedNaluIdx! < _nalus.length
                        ? _nalus[_selectedNaluIdx!]
                        : null,
                    frameIdx: _selectedFrameIdx,
                    frames: _frames,
                    codec: _codec,
                    l: l,
                  ),
                ),
              ],
            ),
            Positioned(
              left: browserW - 4,
              top: 0,
              bottom: 0,
              width: 9,
              child: _ResizableVDivider(
                position: browserW,
                onPositionChanged: (v) => setState(() => _naluBrowserWidth = v),
              ),
            ),
          ],
        );
      },
    );
    return Scaffold(
      body: Column(
        children: [
          // Top bar: order toggle + tab bar
          Container(
            height: _analysisHeaderHeight,
            padding: _analysisHeaderPadding,
            decoration: BoxDecoration(
              border: Border(bottom: BorderSide(color: theme.dividerColor)),
            ),
            child: Row(
              children: [
                // Order toggle
                SizedBox(
                  height: _analysisHeaderControlHeight,
                  child: _OrderToggle(
                    ptsOrder: _ptsOrder,
                    onChanged: (v) => setState(() {
                      _ptsOrder = v;
                      _rebuildSortedFramesCache();
                      _centerChartOnSelectedFrame();
                    }),
                    l: l,
                  ),
                ),
                const Spacer(),
                // Tab bar
                SizedBox(
                  height: _analysisHeaderControlHeight,
                  child: _TabBar(
                    selectedTab: _selectedTab,
                    onTabChanged: (i) => setState(() => _selectedTab = i),
                    l: l,
                  ),
                ),
              ],
            ),
          ),
          Expanded(
            // Top chart and bottom NALU/detail area share a draggable divider.
            child: LayoutBuilder(
              builder: (context, constraints) {
                const dividerH = 10.0;
                final available = (constraints.maxHeight - dividerH).clamp(
                  0.0,
                  double.infinity,
                );
                final compact = available < 280;
                final minTop = compact ? available * 0.28 : 120.0;
                final minBottom = compact ? available * 0.28 : 170.0;
                final maxTop = (available - minBottom).clamp(minTop, available);
                final topH = (available * _topPanelFraction).clamp(
                  minTop,
                  maxTop,
                );
                final bottomH = available - topH;
                return Column(
                  children: [
                    SizedBox(height: topH, child: topChart),
                    SizedBox(
                      height: dividerH,
                      child: _ResizableHDivider(
                        position: topH,
                        minPosition: minTop,
                        maxPosition: maxTop,
                        onPositionChanged: (nextTop) => setState(() {
                          if (available <= 0) return;
                          _topPanelFraction = nextTop / available;
                        }),
                      ),
                    ),
                    SizedBox(height: bottomH, child: bottomPanel),
                  ],
                );
              },
            ),
          ),
        ],
      ),
    );
  }
}

// ===========================================================================
// Top bar widgets
// ===========================================================================

class _OrderToggle extends StatelessWidget {
  final bool ptsOrder;
  final ValueChanged<bool> onChanged;
  final AppLocalizations l;
  const _OrderToggle({
    required this.ptsOrder,
    required this.onChanged,
    required this.l,
  });

  @override
  Widget build(BuildContext context) {
    return ExcludeSemantics(
      child: SegmentedButton<bool>(
        showSelectedIcon: false,
        segments: [
          ButtonSegment(value: true, label: Text(l.analysisPtsOrder)),
          ButtonSegment(value: false, label: Text(l.analysisDtsOrder)),
        ],
        selected: {ptsOrder},
        onSelectionChanged: (s) => onChanged(s.first),
        style: const ButtonStyle(
          visualDensity: VisualDensity.compact,
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          textStyle: WidgetStatePropertyAll(
            TextStyle(fontSize: 12, fontWeight: FontWeight.w700),
          ),
          fixedSize: WidgetStatePropertyAll(
            Size.fromHeight(_analysisHeaderControlHeight),
          ),
        ),
      ),
    );
  }
}

class _TabBar extends StatelessWidget {
  final int selectedTab;
  final ValueChanged<int> onTabChanged;
  final AppLocalizations l;
  const _TabBar({
    required this.selectedTab,
    required this.onTabChanged,
    required this.l,
  });

  @override
  Widget build(BuildContext context) {
    return ExcludeSemantics(
      child: SegmentedButton<int>(
        showSelectedIcon: false,
        segments: [
          ButtonSegment(
            value: 0,
            label: Tooltip(
              message: l.analysisRefPyramid,
              child: const SizedBox(
                width: 28,
                height: 20,
                child: _AnalysisViewIcon(_AnalysisViewIconKind.pyramid),
              ),
            ),
          ),
          ButtonSegment(
            value: 1,
            label: Tooltip(
              message: l.analysisFrameTrend,
              child: const SizedBox(
                width: 28,
                height: 20,
                child: _AnalysisViewIcon(_AnalysisViewIconKind.trend),
              ),
            ),
          ),
        ],
        selected: {selectedTab},
        onSelectionChanged: (s) => onTabChanged(s.first),
        style: const ButtonStyle(
          visualDensity: VisualDensity.compact,
          padding: WidgetStatePropertyAll(EdgeInsets.symmetric(horizontal: 7)),
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          textStyle: WidgetStatePropertyAll(TextStyle(fontSize: 12)),
          fixedSize: WidgetStatePropertyAll(
            Size.fromHeight(_analysisHeaderControlHeight),
          ),
        ),
      ),
    );
  }
}

enum _AnalysisViewIconKind { pyramid, trend }

class _AnalysisViewIcon extends StatelessWidget {
  final _AnalysisViewIconKind kind;

  const _AnalysisViewIcon(this.kind);

  @override
  Widget build(BuildContext context) {
    final color =
        IconTheme.of(context).color ??
        DefaultTextStyle.of(context).style.color ??
        Theme.of(context).colorScheme.onSurface;
    return CustomPaint(painter: _AnalysisViewIconPainter(kind, color));
  }
}

class _AnalysisViewIconPainter extends CustomPainter {
  final _AnalysisViewIconKind kind;
  final Color color;

  const _AnalysisViewIconPainter(this.kind, this.color);

  @override
  void paint(Canvas canvas, Size size) {
    final stroke = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.35
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;
    final fill = Paint()
      ..color = color.withValues(alpha: 0.92)
      ..style = PaintingStyle.fill;

    switch (kind) {
      case _AnalysisViewIconKind.pyramid:
        final p0 = Offset(size.width * 0.18, size.height * 0.76);
        final p1 = Offset(size.width * 0.42, size.height * 0.46);
        final p2 = Offset(size.width * 0.68, size.height * 0.22);
        final p3 = Offset(size.width * 0.84, size.height * 0.58);
        canvas.drawLine(p0, p1, stroke);
        canvas.drawLine(p1, p2, stroke);
        canvas.drawLine(p1, p3, stroke);
        canvas.drawLine(p2, p3, stroke..color = color.withValues(alpha: 0.55));
        for (final p in [p0, p1, p2, p3]) {
          canvas.drawCircle(p, 2.25, fill);
        }

      case _AnalysisViewIconKind.trend:
        final baseY = size.height * 0.76;
        final barW = size.width * 0.085;
        final xs = [
          size.width * 0.20,
          size.width * 0.40,
          size.width * 0.60,
          size.width * 0.80,
        ];
        final hs = [
          size.height * 0.30,
          size.height * 0.50,
          size.height * 0.24,
          size.height * 0.60,
        ];
        for (var i = 0; i < xs.length; i++) {
          canvas.drawRRect(
            RRect.fromRectAndRadius(
              Rect.fromLTWH(xs[i], baseY - hs[i], barW, hs[i]),
              const Radius.circular(1.5),
            ),
            fill,
          );
        }
        final line = Path()
          ..moveTo(size.width * 0.12, size.height * 0.64)
          ..lineTo(size.width * 0.35, size.height * 0.54)
          ..lineTo(size.width * 0.56, size.height * 0.62)
          ..lineTo(size.width * 0.86, size.height * 0.36);
        canvas.drawPath(line, stroke);
    }
  }

  @override
  bool shouldRepaint(covariant _AnalysisViewIconPainter oldDelegate) =>
      kind != oldDelegate.kind || color != oldDelegate.color;
}

// ===========================================================================
// Resizable vertical divider (drag to change left panel width)
// ===========================================================================

class _ResizableVDivider extends StatefulWidget {
  final double position;
  final ValueChanged<double> onPositionChanged;

  const _ResizableVDivider({
    required this.position,
    required this.onPositionChanged,
  });

  @override
  State<_ResizableVDivider> createState() => _ResizableVDividerState();
}

class _ResizableVDividerState extends State<_ResizableVDivider> {
  bool _hovering = false;
  double _excess = 0.0;
  late double _effectivePos;

  @override
  void initState() {
    super.initState();
    _effectivePos = widget.position;
  }

  @override
  void didUpdateWidget(covariant _ResizableVDivider oldWidget) {
    super.didUpdateWidget(oldWidget);
    _effectivePos = widget.position;
  }

  void _onDragStart(_) {
    _excess = 0.0;
    _effectivePos = widget.position;
  }

  void _onDragUpdate(DragUpdateDetails details) {
    final desired = _effectivePos + _excess + details.delta.dx;
    final clamped = desired.clamp(120.0, double.maxFinite);
    _excess = desired - clamped;
    _effectivePos = clamped;
    widget.onPositionChanged(clamped);
  }

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onHorizontalDragStart: _onDragStart,
      onHorizontalDragUpdate: _onDragUpdate,
      child: MouseRegion(
        cursor: SystemMouseCursors.resizeColumn,
        onEnter: (_) => setState(() => _hovering = true),
        onExit: (_) => setState(() => _hovering = false),
        child: SizedBox.expand(
          child: Center(
            child: Container(
              width: _hovering ? 2 : 0,
              color: _hovering
                  ? Theme.of(context).colorScheme.primary
                  : Colors.transparent,
            ),
          ),
        ),
      ),
    );
  }
}

class _ResizableHDivider extends StatefulWidget {
  final double position;
  final double minPosition;
  final double maxPosition;
  final ValueChanged<double> onPositionChanged;

  const _ResizableHDivider({
    required this.position,
    required this.minPosition,
    required this.maxPosition,
    required this.onPositionChanged,
  });

  @override
  State<_ResizableHDivider> createState() => _ResizableHDividerState();
}

class _ResizableHDividerState extends State<_ResizableHDivider> {
  bool _hovering = false;
  double _dragStartGlobalY = 0.0;
  double _dragStartPosition = 0.0;

  void _onDragStart(DragStartDetails details) {
    _dragStartGlobalY = details.globalPosition.dy;
    _dragStartPosition = widget.position;
  }

  void _onDragUpdate(DragUpdateDetails details) {
    final desired =
        _dragStartPosition + details.globalPosition.dy - _dragStartGlobalY;
    final clamped = desired.clamp(widget.minPosition, widget.maxPosition);
    widget.onPositionChanged(clamped);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onVerticalDragStart: _onDragStart,
      onVerticalDragUpdate: _onDragUpdate,
      child: MouseRegion(
        cursor: SystemMouseCursors.resizeRow,
        onEnter: (_) => setState(() => _hovering = true),
        onExit: (_) => setState(() => _hovering = false),
        child: SizedBox.expand(
          child: Center(
            child: SizedBox(
              width: double.infinity,
              height: _hovering ? 2 : 1,
              child: ColoredBox(
                color: _hovering
                    ? theme.colorScheme.primary
                    : theme.colorScheme.outlineVariant,
              ),
            ),
          ),
        ),
      ),
    );
  }
}

// ===========================================================================
// Reference Pyramid — circle nodes + reference arrows + level backgrounds
// Supports zoom (scroll wheel) and pan (scrollbar).
// ===========================================================================

const double _analysisChartLabelW = 66.0;
const double _analysisChartXAxisH = 34.0;

String _formatCompactAxisValue(int value) {
  final abs = value.abs();
  if (abs >= 1000000) return '${(value / 1000000).toStringAsFixed(1)}M';
  if (abs >= 1000) return '${(value / 1000).toStringAsFixed(1)}K';
  return '$value';
}

List<double> _axisTickFractionsForHeight(
  double height, {
  int minTicks = 2,
  int maxTicks = 5,
  double minGap = 44.0,
}) {
  if (height <= 0) return const [];
  final tickCount = ((height / minGap).floor() + 1)
      .clamp(minTicks, maxTicks)
      .toInt();
  if (tickCount <= 1) return const [0.0];
  return [for (var i = 0; i < tickCount; i++) i / (tickCount - 1)];
}

void _drawFrameXAxis({
  required Canvas canvas,
  required Size size,
  required double axisTop,
  required double labelW,
  required List<FrameInfo> frames,
  required int visibleStart,
  required int visibleEnd,
  required bool ptsOrder,
  required double Function(int frameIdx) xForFrame,
}) {
  if (axisTop >= size.height || visibleStart >= visibleEnd) return;

  final axisH = size.height - axisTop;
  if (axisH < 26) return;

  final plotLeft = labelW;
  final plotRight = size.width;
  final plotW = (plotRight - plotLeft).clamp(0.0, double.infinity);
  if (plotW <= 0) return;

  final axisPaint = Paint()
    ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.12)
    ..strokeWidth = 1.0;
  final tickPaint = Paint()
    ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.10)
    ..strokeWidth = 1.0;
  canvas.drawLine(Offset(0, axisTop), Offset(size.width, axisTop), axisPaint);
  canvas.drawLine(
    Offset(labelW, axisTop),
    Offset(labelW, size.height),
    axisPaint,
  );

  const labelStyle = TextStyle(
    color: Color(0xFFFFFFFF),
    fontSize: 10,
    fontWeight: FontWeight.w600,
  );
  const valueStyle = TextStyle(color: Color(0xCCFFFFFF), fontSize: 10);
  final axisName = ptsOrder ? 'PTS' : 'DTS';
  final leftTp = TextPainter(
    text: TextSpan(
      children: [
        const TextSpan(text: 'Index\n', style: labelStyle),
        TextSpan(text: axisName, style: valueStyle),
      ],
    ),
    textAlign: TextAlign.right,
    textDirection: TextDirection.ltr,
  )..layout(maxWidth: labelW - 8);
  leftTp.paint(canvas, Offset(labelW - leftTp.width - 6, axisTop + 4));

  final visibleCount = visibleEnd - visibleStart;
  final maxTicks = visibleCount == 1
      ? 1
      : (plotW / 92).floor().clamp(2, visibleCount);
  final step = (visibleCount / maxTicks).ceil().clamp(1, visibleCount);

  var lastRight = plotLeft - 8;
  for (var i = visibleStart; i < visibleEnd; i += step) {
    if (i < 0 || i >= frames.length) continue;
    final x = xForFrame(i);
    if (x < plotLeft || x > plotRight) continue;

    final f = frames[i];
    final value = ptsOrder ? f.pts : f.dts;
    final line1 = '#$i';
    final line2 = _formatCompactAxisValue(value);
    final tickTp = TextPainter(
      text: TextSpan(
        children: [
          TextSpan(text: '$line1\n', style: labelStyle),
          TextSpan(text: line2, style: valueStyle),
        ],
      ),
      textAlign: TextAlign.center,
      textDirection: TextDirection.ltr,
    )..layout();
    final minDrawX = plotLeft + 2;
    final maxDrawX = (plotRight - tickTp.width - 2).clamp(
      minDrawX,
      double.infinity,
    );
    final drawX = (x - tickTp.width / 2).clamp(minDrawX, maxDrawX);
    if (drawX < lastRight + 8) continue;

    canvas.drawLine(Offset(x, axisTop), Offset(x, axisTop + 4), tickPaint);
    tickTp.paint(canvas, Offset(drawX, axisTop + 4));
    lastRight = drawX + tickTp.width;
  }
}

class _ReferencePyramidView extends StatefulWidget {
  final List<FrameInfo> frames;
  final int currentIdx;
  final int? selectedFrameIdx;
  final Map<int, List<int>> pocToIndices;
  final ValueChanged<int?> onFrameSelected;
  final double viewStart;
  final double viewEnd;
  final bool ptsOrder;
  final ValueChanged<double> onZoom;
  final ValueChanged<double> onPan;
  final AppLocalizations l;
  const _ReferencePyramidView({
    required this.frames,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.pocToIndices,
    required this.onFrameSelected,
    required this.viewStart,
    required this.viewEnd,
    required this.ptsOrder,
    required this.onZoom,
    required this.onPan,
    required this.l,
  });

  @override
  State<_ReferencePyramidView> createState() => _ReferencePyramidViewState();
}

class _ReferencePyramidViewState extends State<_ReferencePyramidView> {
  _RefPyramidPainter? _lastPainter;
  Offset? _hoverPosition;

  @override
  Widget build(BuildContext context) {
    if (widget.frames.isEmpty) {
      return Center(child: Text(widget.l.analysisNoFrameData));
    }
    return Column(
      children: [
        Expanded(
          child: Listener(
            onPointerSignal: (signal) {
              if (signal is PointerScrollEvent) {
                widget.onZoom(signal.scrollDelta.dy);
              }
            },
            child: GestureDetector(
              behavior: HitTestBehavior.opaque,
              onTapUp: (details) {
                final rects = _lastPainter?.frameRects ?? [];
                for (final (gi, rect) in rects) {
                  if (rect.contains(details.localPosition)) {
                    widget.onFrameSelected(
                      widget.selectedFrameIdx == gi ? null : gi,
                    );
                    return;
                  }
                }
                widget.onFrameSelected(null);
              },
              child: Builder(
                builder: (chartContext) => MouseRegion(
                  onExit: (_) => setState(() => _hoverPosition = null),
                  onHover: (event) {
                    final box = chartContext.findRenderObject() as RenderBox;
                    setState(() {
                      _hoverPosition = box.globalToLocal(event.position);
                    });
                  },
                  child: CustomPaint(
                    painter: _lastPainter = _RefPyramidPainter(
                      frames: widget.frames,
                      currentIdx: widget.currentIdx,
                      selectedFrameIdx: widget.selectedFrameIdx,
                      pocToIndices: widget.pocToIndices,
                      viewStart: widget.viewStart,
                      viewEnd: widget.viewEnd,
                      ptsOrder: widget.ptsOrder,
                      hoverPosition: _hoverPosition,
                    ),
                    size: Size.infinite,
                  ),
                ),
              ),
            ),
          ),
        ),
        _ChartScrollbar(
          total: widget.frames.length.toDouble(),
          viewStart: widget.viewStart,
          viewEnd: widget.viewEnd,
          onPan: widget.onPan,
        ),
      ],
    );
  }
}

class _RefPyramidPainter extends CustomPainter {
  final List<FrameInfo> frames;
  final int currentIdx;
  final int? selectedFrameIdx;
  final Map<int, List<int>> pocToIndices;
  final double viewStart;
  final double viewEnd;
  final bool ptsOrder;
  final Offset? hoverPosition;

  /// Populated during paint() — used by parent for hit-testing.
  List<(int, Rect)> frameRects = [];

  _RefPyramidPainter({
    required this.frames,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.pocToIndices,
    required this.viewStart,
    required this.viewEnd,
    required this.ptsOrder,
    this.hoverPosition,
  });

  // Pre-allocated Paint objects — reused across draw calls within paint()
  static final _bgPaint = Paint();
  static final _cursorPaint = Paint();
  static final _arrowLinePaint = Paint()
    ..style = PaintingStyle.stroke
    ..strokeCap = StrokeCap.round;
  static final _arrowHeadPaint = Paint();
  static final _fillPaint = Paint();
  static final _strokePaint = Paint()..style = PaintingStyle.stroke;

  @override
  void paint(Canvas canvas, Size size) {
    if (frames.isEmpty) return;

    final visibleStart = viewStart.floor().clamp(0, frames.length - 1);
    final visibleEnd = (viewEnd.ceil() + 1).clamp(0, frames.length);
    if (visibleStart >= visibleEnd) return;

    // --- Layout ---
    int maxTid = 0;
    for (var i = visibleStart; i < visibleEnd; i++) {
      if (frames[i].temporalId > maxTid) maxTid = frames[i].temporalId;
    }
    final axisH = size.height >= 96 ? _analysisChartXAxisH : 0.0;
    final chartH = (size.height - axisH).clamp(1.0, double.infinity);
    final numLevels = maxTid + 1;
    final rowH = chartH / numLevels;
    final labelW = _analysisChartLabelW;
    final usableW = (size.width - labelW).clamp(0.0, double.infinity);
    final span = viewEnd - viewStart;
    final circleR = (rowH * 0.3).clamp(6.0, 20.0);
    final plotRect = Rect.fromLTWH(labelW, 0, usableW, chartH);
    final centerPad = circleR + 2;
    final centerW = (usableW - centerPad * 2).clamp(1.0, double.infinity);

    final positions = <int, Offset>{}; // globalIdx → position
    for (var i = visibleStart; i < visibleEnd; i++) {
      final frac = (i - viewStart) / span;
      final x = labelW + centerPad + frac * centerW;
      final y = chartH - (frames[i].temporalId + 0.5) * rowH;
      positions[i] = Offset(x, y);
    }
    frameRects = [
      for (final e in positions.entries) ...[
        if (Rect.fromCircle(
          center: e.value,
          radius: circleR,
        ).overlaps(plotRect))
          (
            e.key,
            Rect.fromCircle(
              center: e.value,
              radius: circleR,
            ).intersect(plotRect),
          ),
      ],
    ];

    // --- Level backgrounds ---
    final levelLabelStep = rowH >= 16 ? 1 : (16 / rowH).ceil();
    for (var tid = 0; tid <= maxTid; tid++) {
      final top = chartH - (tid + 1) * rowH;
      final alpha = 0.03 + (maxTid - tid) * 0.025;
      _bgPaint.color = const Color(0xFFFFFFFF).withValues(alpha: alpha);
      canvas.drawRect(Rect.fromLTWH(0, top, size.width, rowH), _bgPaint);
      final showLevelLabel =
          tid == 0 || tid == maxTid || tid % levelLabelStep == 0;
      if (!showLevelLabel) continue;
      final tp = TextPainter(
        text: TextSpan(
          text: 'L$tid',
          style: const TextStyle(
            color: Color(0xFFFFFFFF),
            fontSize: 10,
            fontWeight: FontWeight.w600,
            letterSpacing: 0.5,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      final drawY = (top + rowH / 2 - tp.height / 2).clamp(
        0.0,
        chartH - tp.height,
      );
      tp.paint(canvas, Offset(4, drawY));
    }
    canvas.drawLine(
      Offset(labelW, 0),
      Offset(labelW, chartH),
      Paint()
        ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.10)
        ..strokeWidth = 1,
    );

    canvas.save();
    canvas.clipRect(plotRect);
    // --- Current playback cursor ---
    if (currentIdx >= 0 && positions.containsKey(currentIdx)) {
      final cx = positions[currentIdx]!.dx;
      _cursorPaint.color = const Color(0xFFFFFFFF).withValues(alpha: 0.15);
      _cursorPaint.strokeWidth = 1;
      canvas.drawLine(Offset(cx, 0), Offset(cx, chartH), _cursorPaint);
    }

    // --- Reference arrows ---
    // pocToIndices is pre-built by parent (cached across repaints).

    int? nearestRefIdx(int refPoc, int sourceIdx) {
      final indices = pocToIndices[refPoc];
      if (indices == null || indices.isEmpty) return null;
      var best = indices[0];
      for (final idx in indices) {
        if ((idx - sourceIdx).abs() < (best - sourceIdx).abs()) best = idx;
      }
      return best;
    }

    Offset posFor(int idx) {
      if (positions.containsKey(idx)) return positions[idx]!;
      final frac = (idx - viewStart) / span;
      final x = labelW + centerPad + frac * centerW;
      final y = chartH - (frames[idx].temporalId + 0.5) * rowH;
      return Offset(x, y);
    }

    bool endpointVisible(int idx) {
      if (idx < 0 || idx >= frames.length) return false;
      return Rect.fromCircle(
        center: posFor(idx),
        radius: circleR,
      ).overlaps(plotRect);
    }

    List<int> refsFor(int sourceIdx) {
      final f = frames[sourceIdx];
      final refs = <int>[];
      for (var j = 0; j < f.numRefL0 && j < f.refPocsL0.length; j++) {
        final ri = nearestRefIdx(f.refPocsL0[j], sourceIdx);
        if (ri != null && ri >= 0 && ri < frames.length) refs.add(ri);
      }
      for (var j = 0; j < f.numRefL1 && j < f.refPocsL1.length; j++) {
        final ri = nearestRefIdx(f.refPocsL1[j], sourceIdx);
        if (ri != null && ri >= 0 && ri < frames.length) refs.add(ri);
      }
      return refs;
    }

    // Collect the selected frame's transitive reference chain for highlighting.
    final selectedChainEdges = <String>{};
    final selectedChainNodes = <int>{};
    if (selectedFrameIdx != null &&
        selectedFrameIdx! >= 0 &&
        selectedFrameIdx! < frames.length) {
      void visitReferenceChain(int sourceIdx) {
        if (!selectedChainNodes.add(sourceIdx)) return;
        for (final refIdx in refsFor(sourceIdx)) {
          selectedChainEdges.add('$sourceIdx:$refIdx');
          visitReferenceChain(refIdx);
        }
      }

      visitReferenceChain(selectedFrameIdx!);
    }

    // Helper: get fill color for a frame's circle (same logic as circle drawing)
    Color frameFillColor(FrameInfo f) {
      if (f.sliceType == 2) return const Color(0xFFFF4D4F); // I: red
      if (f.sliceType == 0 && f.numRefL1 > 0) {
        return const Color(0xFF1890FF); // B bidir: blue
      }
      return const Color(0xFF52C41A); // B uni / P: green
    }

    // Draw reference edges that affect the viewport. This includes offscreen
    // sources/targets when the other endpoint is visible, preventing lines from
    // popping in only when an offscreen source node enters the viewport.
    final drawnEdges = <String>{};
    for (var i = 0; i < frames.length; i++) {
      final f = frames[i];
      if (f.numRefL0 == 0 && f.numRefL1 == 0) continue;
      final from = posFor(i);
      final sourceVisible = endpointVisible(i);

      for (final ri in refsFor(i)) {
        final edgeKey = '$i:$ri';
        if (!drawnEdges.add(edgeKey)) continue;

        final to = posFor(ri);
        final targetVisible = endpointVisible(ri);
        final isSelLine =
            selectedFrameIdx != null && selectedChainEdges.contains(edgeKey);
        if (!sourceVisible && !targetVisible && !isSelLine) continue;
        if (!_segmentIntersectsRect(from, to, plotRect)) continue;

        final lineW = isSelLine
            ? 2.5
            : (sourceVisible && targetVisible ? 1 : 0.8);
        final baseAlpha = sourceVisible && targetVisible ? 0.5 : 0.36;
        final arrowAlpha = isSelLine
            ? 1.0
            : (selectedFrameIdx != null ? baseAlpha * 0.4 : baseAlpha);
        final arrowColor = frameFillColor(
          frames[ri],
        ).withValues(alpha: arrowAlpha);
        _drawArrow(canvas, from, to, arrowColor, lineW.toDouble(), circleR);
      }
    }
    // --- Frame circles ---
    final related = <int>{};
    if (selectedFrameIdx != null) {
      related.addAll(selectedChainNodes);
    }

    // Cache label TextPainters (only 3 variants: I, P, B)
    TextPainter? labelI, labelP, labelB;
    if (circleR >= 8) {
      labelI = TextPainter(
        text: const TextSpan(
          text: 'I',
          style: TextStyle(
            color: Color(0xFFFFFFFF),
            fontSize: 14,
            fontWeight: FontWeight.bold,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      labelP = TextPainter(
        text: const TextSpan(
          text: 'P',
          style: TextStyle(
            color: Color(0xFFFFFFFF),
            fontSize: 14,
            fontWeight: FontWeight.bold,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      labelB = TextPainter(
        text: const TextSpan(
          text: 'B',
          style: TextStyle(
            color: Color(0xFF0050B3),
            fontSize: 14,
            fontWeight: FontWeight.bold,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
    }

    for (final entry in positions.entries) {
      final i = entry.key;
      final pos = entry.value;
      final f = frames[i];
      final isSelected = i == selectedFrameIdx;
      final isRelated = related.contains(i);

      // VBS2 slice_type: 0=B, 1=P, 2=I (see binary_types.h)
      // B with l1=0 (unidirectional) uses green color but still labeled B
      final Color fill, stroke;
      if (f.sliceType == 2) {
        fill = const Color(0xFFFF4D4F);
        stroke = const Color(0xFFCF1322);
      } else if (f.sliceType == 0 && f.numRefL1 > 0) {
        fill = const Color(0xFFE6F7FF);
        stroke = const Color(0xFF1890FF);
      } else if (f.sliceType == 0) {
        // B with l1==0 (unidirectional): green color
        fill = const Color(0xFF52C41A);
        stroke = const Color(0xFF389E0D);
      } else {
        // P (sliceType==1)
        fill = const Color(0xFF52C41A);
        stroke = const Color(0xFF389E0D);
      }

      final sw2 = isSelected ? 4.5 : (isRelated ? 3.5 : 2.5);
      final r = isSelected ? circleR + 2 : circleR;

      _fillPaint.color = fill;
      canvas.drawCircle(pos, r, _fillPaint);
      _strokePaint.color = stroke;
      _strokePaint.strokeWidth = sw2;
      canvas.drawCircle(pos, r, _strokePaint);
      if (isSelected) {
        _strokePaint.color = const Color(0xFFFFFFFF);
        _strokePaint.strokeWidth = 2.0;
        canvas.drawCircle(pos, r + sw2 / 2 + 1.5, _strokePaint);
      }

      if (circleR >= 8) {
        // Label: always show actual slice type (I/P/B), color distinguishes ref direction
        final TextPainter tp;
        if (f.sliceType == 2) {
          tp = labelI!;
        } else if (f.sliceType == 0) {
          tp = labelB!;
        } else {
          tp = labelP!;
        }
        tp.paint(canvas, pos - Offset(tp.width / 2, tp.height / 2));
      }
    }
    _drawHoverTooltip(canvas, size, plotRect, positions, circleR);
    canvas.restore();

    _drawFrameXAxis(
      canvas: canvas,
      size: size,
      axisTop: chartH,
      labelW: labelW,
      frames: frames,
      visibleStart: visibleStart,
      visibleEnd: visibleEnd,
      ptsOrder: ptsOrder,
      xForFrame: (idx) {
        final frac = (idx - viewStart) / span;
        return labelW + centerPad + frac * centerW;
      },
    );
  }

  void _drawHoverTooltip(
    Canvas canvas,
    Size size,
    Rect plotRect,
    Map<int, Offset> positions,
    double circleR,
  ) {
    final hover = hoverPosition;
    if (hover == null || !plotRect.contains(hover)) return;

    int? frameIdx;
    for (final (idx, rect) in frameRects) {
      if (rect.contains(hover)) {
        frameIdx = idx;
        break;
      }
    }
    if (frameIdx == null || frameIdx < 0 || frameIdx >= frames.length) return;

    final f = frames[frameIdx];
    final pos = positions[frameIdx] ?? hover;
    final sliceLabel = switch (f.sliceType) {
      2 => 'I',
      1 => 'P',
      _ => f.numRefL1 > 0 ? 'B' : 'B(uni)',
    };
    final lines = [
      '#$frameIdx  $sliceLabel  POC ${f.poc}',
      'Size: ${_FrameTrendPainter._formatBytes(f.packetSize)}',
      'QP: ${f.avgQp}',
      'PTS: ${f.pts}',
      'DTS: ${f.dts}',
    ];
    const tipStyle = TextStyle(color: Color(0xFFFFFFFF), fontSize: 10);
    final tipPainters = lines
        .map(
          (line) => TextPainter(
            text: TextSpan(text: line, style: tipStyle),
            textDirection: TextDirection.ltr,
          )..layout(),
        )
        .toList();
    final tipW =
        tipPainters.map((t) => t.width).reduce((a, b) => a > b ? a : b) + 12;
    final tipH = tipPainters.fold(0.0, (sum, t) => sum + t.height) + 10;

    var tipX = pos.dx + circleR + 8;
    if (tipX + tipW > plotRect.right) tipX = pos.dx - circleR - tipW - 8;
    final minTipX = plotRect.left + 4;
    final maxTipX = (plotRect.right - tipW - 4).clamp(minTipX, double.infinity);
    tipX = tipX.clamp(minTipX, maxTipX);
    final maxTipY = (size.height - tipH - 4).clamp(4.0, double.infinity);
    final tipY = (pos.dy - tipH - circleR - 8).clamp(4.0, maxTipY);

    final rrect = RRect.fromRectAndRadius(
      Rect.fromLTWH(tipX, tipY, tipW, tipH),
      const Radius.circular(4),
    );
    canvas.drawRRect(rrect, Paint()..color = const Color(0xCC1A1A2E));
    canvas.drawRRect(
      rrect,
      Paint()
        ..color = const Color(0x44FFFFFF)
        ..style = PaintingStyle.stroke
        ..strokeWidth = 0.5,
    );

    var offsetY = tipY + 5;
    for (final tp in tipPainters) {
      tp.paint(canvas, Offset(tipX + 6, offsetY));
      offsetY += tp.height;
    }
  }

  void _drawArrow(
    Canvas canvas,
    Offset from,
    Offset to,
    Color color,
    double strokeWidth,
    double circleR,
  ) {
    final delta = to - from;
    final dist = delta.distance;
    if (dist < circleR * 2 + 2) return;

    final unit = delta / dist;
    final p1 = from + unit * circleR;
    final p2 = to - unit * (circleR + 6);

    _arrowLinePaint.color = color;
    _arrowLinePaint.strokeWidth = strokeWidth;
    canvas.drawLine(p1, p2, _arrowLinePaint);

    final arrowLen = strokeWidth * 3.5;
    final perp = Offset(-unit.dy, unit.dx);
    final tip = to - unit * circleR;
    final a1 = tip - unit * arrowLen + perp * arrowLen * 0.45;
    final a2 = tip - unit * arrowLen - perp * arrowLen * 0.45;

    _arrowHeadPaint.color = color;
    canvas.drawPath(
      Path()
        ..moveTo(tip.dx, tip.dy)
        ..lineTo(a1.dx, a1.dy)
        ..lineTo(a2.dx, a2.dy)
        ..close(),
      _arrowHeadPaint,
    );
  }

  bool _segmentIntersectsRect(Offset a, Offset b, Rect rect) {
    if (rect.contains(a) || rect.contains(b)) return true;
    final left = a.dx < b.dx ? a.dx : b.dx;
    final right = a.dx > b.dx ? a.dx : b.dx;
    final top = a.dy < b.dy ? a.dy : b.dy;
    final bottom = a.dy > b.dy ? a.dy : b.dy;
    if (right < rect.left ||
        left > rect.right ||
        bottom < rect.top ||
        top > rect.bottom) {
      return false;
    }

    final topLeft = rect.topLeft;
    final topRight = rect.topRight;
    final bottomRight = rect.bottomRight;
    final bottomLeft = rect.bottomLeft;
    return _segmentsIntersect(a, b, topLeft, topRight) ||
        _segmentsIntersect(a, b, topRight, bottomRight) ||
        _segmentsIntersect(a, b, bottomRight, bottomLeft) ||
        _segmentsIntersect(a, b, bottomLeft, topLeft);
  }

  bool _segmentsIntersect(Offset a, Offset b, Offset c, Offset d) {
    final o1 = _orientation(a, b, c);
    final o2 = _orientation(a, b, d);
    final o3 = _orientation(c, d, a);
    final o4 = _orientation(c, d, b);
    if (o1 == 0 && _onSegment(a, c, b)) return true;
    if (o2 == 0 && _onSegment(a, d, b)) return true;
    if (o3 == 0 && _onSegment(c, a, d)) return true;
    if (o4 == 0 && _onSegment(c, b, d)) return true;
    return (o1 > 0) != (o2 > 0) && (o3 > 0) != (o4 > 0);
  }

  double _orientation(Offset a, Offset b, Offset c) {
    final v = (b.dy - a.dy) * (c.dx - b.dx) - (b.dx - a.dx) * (c.dy - b.dy);
    return v.abs() < 0.0001 ? 0 : v;
  }

  bool _onSegment(Offset a, Offset b, Offset c) {
    return b.dx <= (a.dx > c.dx ? a.dx : c.dx) + 0.0001 &&
        b.dx + 0.0001 >= (a.dx < c.dx ? a.dx : c.dx) &&
        b.dy <= (a.dy > c.dy ? a.dy : c.dy) + 0.0001 &&
        b.dy + 0.0001 >= (a.dy < c.dy ? a.dy : c.dy);
  }

  @override
  bool shouldRepaint(covariant _RefPyramidPainter old) =>
      frames.length != old.frames.length ||
      currentIdx != old.currentIdx ||
      selectedFrameIdx != old.selectedFrameIdx ||
      viewStart != old.viewStart ||
      viewEnd != old.viewEnd ||
      ptsOrder != old.ptsOrder ||
      hoverPosition != old.hoverPosition;
}

// ===========================================================================
// Frame Trend — zoomable / pannable bar chart
// ===========================================================================

const double _frameTrendLabelW = _analysisChartLabelW;

enum _FrameTrendAxis { frameSize, qp }

class _FrameTrendView extends StatefulWidget {
  final List<FrameInfo> frames;
  final int currentIdx;
  final int? selectedFrameIdx;
  final double viewStart;
  final double viewEnd;
  final double frameSizeAxisZoom;
  final double qpAxisZoom;
  final bool ptsOrder;
  final ValueChanged<double> onZoom;
  final void Function(_FrameTrendAxis axis, double scrollDelta) onAxisZoom;
  final ValueChanged<double> onPan;
  final ValueChanged<int?> onFrameSelected;
  final AppLocalizations l;
  const _FrameTrendView({
    required this.frames,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.viewStart,
    required this.viewEnd,
    required this.frameSizeAxisZoom,
    required this.qpAxisZoom,
    required this.ptsOrder,
    required this.onZoom,
    required this.onAxisZoom,
    required this.onPan,
    required this.onFrameSelected,
    required this.l,
  });

  @override
  State<_FrameTrendView> createState() => _FrameTrendViewState();
}

class _FrameTrendViewState extends State<_FrameTrendView> {
  double? _hoverX; // null = not hovering

  @override
  Widget build(BuildContext context) {
    final w = widget;
    if (w.frames.isEmpty) {
      return Center(child: Text(w.l.analysisNoFrameData));
    }
    return Column(
      children: [
        Expanded(
          child: Builder(
            builder: (chartContext) => MouseRegion(
              onExit: (_) => setState(() => _hoverX = null),
              child: Listener(
                onPointerSignal: (signal) {
                  if (signal is PointerScrollEvent) {
                    final box = chartContext.findRenderObject() as RenderBox;
                    final local = box.globalToLocal(signal.position);
                    final axisH = box.size.height >= 96
                        ? _analysisChartXAxisH
                        : 0.0;
                    final chartH = (box.size.height - axisH).clamp(
                      1.0,
                      double.infinity,
                    );
                    final upperH = chartH * 0.58;
                    final lowerTop = upperH + chartH * 0.05;
                    final lowerH = chartH * 0.32;
                    if (local.dx < _frameTrendLabelW) {
                      if (local.dy <= upperH) {
                        w.onAxisZoom(
                          _FrameTrendAxis.frameSize,
                          signal.scrollDelta.dy,
                        );
                      } else if (local.dy >= lowerTop &&
                          local.dy <= lowerTop + lowerH) {
                        w.onAxisZoom(_FrameTrendAxis.qp, signal.scrollDelta.dy);
                      }
                    } else {
                      w.onZoom(signal.scrollDelta.dy);
                    }
                  }
                },
                child: GestureDetector(
                  behavior: HitTestBehavior.opaque,
                  onTapUp: (details) {
                    final box = chartContext.findRenderObject() as RenderBox;
                    final localX = box.globalToLocal(details.globalPosition).dx;
                    final chartW = box.size.width - _frameTrendLabelW;
                    if (chartW <= 0 || localX < _frameTrendLabelW) {
                      w.onFrameSelected(null);
                      return;
                    }
                    final span = w.viewEnd - w.viewStart;
                    final idx =
                        (w.viewStart +
                                ((localX - _frameTrendLabelW) / chartW) * span)
                            .round()
                            .clamp(0, w.frames.length - 1);
                    w.onFrameSelected(w.selectedFrameIdx == idx ? null : idx);
                  },
                  onPanUpdate: (details) {
                    final box = chartContext.findRenderObject() as RenderBox;
                    final local = box.globalToLocal(details.globalPosition);
                    setState(() => _hoverX = local.dx);
                  },
                  child: MouseRegion(
                    onHover: (e) {
                      final box = chartContext.findRenderObject() as RenderBox;
                      final local = box.globalToLocal(e.position);
                      setState(() => _hoverX = local.dx);
                    },
                    child: CustomPaint(
                      painter: _FrameTrendPainter(
                        frames: w.frames,
                        currentIdx: w.currentIdx,
                        selectedFrameIdx: w.selectedFrameIdx,
                        viewStart: w.viewStart,
                        viewEnd: w.viewEnd,
                        frameSizeAxisZoom: w.frameSizeAxisZoom,
                        qpAxisZoom: w.qpAxisZoom,
                        ptsOrder: w.ptsOrder,
                        hoverX: _hoverX,
                      ),
                      size: Size.infinite,
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
        _ChartScrollbar(
          total: w.frames.length.toDouble(),
          viewStart: w.viewStart,
          viewEnd: w.viewEnd,
          onPan: w.onPan,
        ),
      ],
    );
  }
}

class _FrameTrendPainter extends CustomPainter {
  final List<FrameInfo> frames;
  final int currentIdx;
  final int? selectedFrameIdx;
  final double viewStart;
  final double viewEnd;
  final double frameSizeAxisZoom;
  final double qpAxisZoom;
  final bool ptsOrder;
  final double? hoverX;

  _FrameTrendPainter({
    required this.frames,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.viewStart,
    required this.viewEnd,
    required this.frameSizeAxisZoom,
    required this.qpAxisZoom,
    required this.ptsOrder,
    this.hoverX,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (frames.isEmpty) return;

    final visibleStart = viewStart.floor().clamp(0, frames.length - 1);
    final visibleEnd = (viewEnd.ceil() + 1).clamp(0, frames.length);
    if (visibleStart >= visibleEnd) return;

    final count = visibleEnd - visibleStart;
    final span = viewEnd - viewStart;

    final axisH = size.height >= 96 ? _analysisChartXAxisH : 0.0;
    final chartH = (size.height - axisH).clamp(1.0, double.infinity);
    final labelW = _frameTrendLabelW;
    final chartW = (size.width - labelW).clamp(0.0, double.infinity);
    if (chartW <= 0) return;
    final upperH = chartH * 0.58;
    final lowerH = chartH * 0.32;
    final gapH = chartH * 0.05;
    final lowerTop = upperH + gapH;
    final plotRect = Rect.fromLTWH(labelW, 0, chartW, chartH);

    final barW = (chartW / count).clamp(2.0, 40.0);

    // Find range for visible frames
    int maxPacketSize = 1;
    int minQp = 63, maxQp = 0;
    for (var i = visibleStart; i < visibleEnd; i++) {
      if (frames[i].packetSize > maxPacketSize) {
        maxPacketSize = frames[i].packetSize;
      }
      if (frames[i].avgQp < minQp) minQp = frames[i].avgQp;
      if (frames[i].avgQp > maxQp) maxQp = frames[i].avgQp;
    }
    final autoSizeMax = maxPacketSize.toDouble().clamp(1.0, double.infinity);
    final sizeAxisMax = (autoSizeMax / frameSizeAxisZoom).clamp(
      1.0,
      double.infinity,
    );

    final autoQpLow = (minQp / 5).floor() * 5.0;
    final autoQpHigh = ((maxQp / 5).floor() + 1) * 5.0;
    final autoQpRange = (autoQpHigh - autoQpLow).clamp(5.0, 63.0);
    final qpRange = (autoQpRange / qpAxisZoom).clamp(1.0, 63.0);
    final qpCenter = ((minQp + maxQp) / 2).clamp(0.0, 63.0);
    var qpLow = qpCenter - qpRange / 2;
    var qpHigh = qpCenter + qpRange / 2;
    if (qpLow < 0) {
      qpHigh -= qpLow;
      qpLow = 0;
    }
    if (qpHigh > 63) {
      qpLow -= qpHigh - 63;
      qpHigh = 63;
      if (qpLow < 0) qpLow = 0;
    }
    final effectiveQpRange = (qpHigh - qpLow).clamp(1.0, 63.0);

    // Label style — same as pyramid level labels
    const labelStyle = TextStyle(
      color: Color(0xFFFFFFFF),
      fontSize: 10,
      fontWeight: FontWeight.w600,
      letterSpacing: 0.5,
    );
    final axisPaint = Paint()
      ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.10)
      ..strokeWidth = 1.0;
    final gridPaint = Paint()
      ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.06)
      ..strokeWidth = 0.5;
    canvas.drawLine(Offset(labelW, 0), Offset(labelW, chartH), axisPaint);
    canvas.drawLine(
      Offset(0, upperH + gapH / 2),
      Offset(size.width, upperH + gapH / 2),
      Paint()
        ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.14)
        ..strokeWidth = 1.0,
    );

    // --- Packet size axis labels (upper) ---
    for (final yFrac in _axisTickFractionsForHeight(upperH, maxTicks: 4)) {
      final text = _formatBytes((sizeAxisMax * yFrac).round());
      final y = upperH * (1 - yFrac);
      canvas.drawLine(Offset(labelW, y), Offset(size.width, y), gridPaint);
      final tp = _axisLabelPainter(
        _byteAxisLabelLines(text),
        labelStyle,
        labelW - 8,
      );
      // Clamp to avoid clipping at top
      final drawY = (y - tp.height / 2).clamp(0.0, upperH - tp.height);
      tp.paint(canvas, Offset(labelW - 4 - tp.width, drawY));
    }

    // --- QP axis labels (lower) ---
    for (final yFrac in _axisTickFractionsForHeight(lowerH, maxTicks: 4)) {
      final value = qpLow + effectiveQpRange * yFrac;
      final y = lowerTop + lowerH * (1 - yFrac);
      canvas.drawLine(Offset(labelW, y), Offset(size.width, y), gridPaint);
      final tp = TextPainter(
        text: TextSpan(text: _formatQpLabel(value), style: labelStyle),
        textDirection: TextDirection.ltr,
      )..layout();
      final drawY = (y - tp.height / 2).clamp(
        lowerTop,
        lowerTop + lowerH - tp.height,
      );
      tp.paint(canvas, Offset(labelW - 4 - tp.width, drawY));
    }

    canvas.save();
    canvas.clipRect(plotRect);

    // --- Frame size bars ---
    final barPaint = Paint()..style = PaintingStyle.fill;
    final selStroke = Paint()
      ..color = const Color(0xFFFFFFFF)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2.0;
    for (var i = visibleStart; i < visibleEnd; i++) {
      final f = frames[i];
      final frac = (i - viewStart) / span;
      final x = labelW + frac * chartW;
      final h = ((f.packetSize / sizeAxisMax).clamp(0.0, 1.0)) * upperH;

      barPaint.color = f.keyframe == 1
          ? const Color(0xFFFF5252)
          : const Color(0xFF42A5F5);
      final rect = Rect.fromLTWH(x, upperH - h, barW - 1, h);
      canvas.drawRect(rect, barPaint);

      if (i == selectedFrameIdx) {
        canvas.drawRect(rect.inflate(1), selStroke);
      }
    }

    // --- QP line ---
    final qpPaint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5
      ..color = const Color(0xFFFFB74D);
    final qpPath = Path();
    bool first = true;
    for (var i = visibleStart; i < visibleEnd; i++) {
      final f = frames[i];
      final frac = (i - viewStart) / span;
      final x = labelW + frac * chartW + barW / 2;
      final normalizedQp = ((f.avgQp - qpLow) / effectiveQpRange).clamp(
        0.0,
        1.0,
      );
      final y = lowerTop + lowerH * (1 - normalizedQp);
      if (first) {
        qpPath.moveTo(x, y);
        first = false;
      } else {
        qpPath.lineTo(x, y);
      }
    }
    canvas.drawPath(qpPath, qpPaint);

    // --- Playback cursor ---
    if (currentIdx >= 0 && currentIdx < frames.length) {
      final frac = (currentIdx - viewStart) / span;
      final cx = labelW + frac * chartW;
      canvas.drawLine(
        Offset(cx, 0),
        Offset(cx, chartH),
        Paint()
          ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.5)
          ..strokeWidth = 1,
      );
    }

    // --- Hover crosshair + tooltip ---
    if (hoverX != null && hoverX! >= labelW) {
      final relX = hoverX! - labelW;
      final frameFrac = relX / chartW;
      final frameIdx = (viewStart + frameFrac * span).round().clamp(
        visibleStart,
        visibleEnd - 1,
      );

      final crossX =
          labelW + ((frameIdx - viewStart) / span) * chartW + barW / 2;
      canvas.drawLine(
        Offset(crossX, 0),
        Offset(crossX, chartH),
        Paint()
          ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.3)
          ..strokeWidth = 1,
      );

      final f = frames[frameIdx];
      final sliceLabel = switch (f.sliceType) {
        2 => 'I',
        1 => 'P',
        _ => f.numRefL1 > 0 ? 'B' : 'B(uni)',
      };
      final lines = [
        '#$frameIdx  $sliceLabel  POC ${f.poc}',
        'Size: ${_formatBytes(f.packetSize)}',
        'QP: ${f.avgQp}',
        'PTS: ${f.pts}',
        'DTS: ${f.dts}',
      ];
      final tipStyle = const TextStyle(color: Color(0xFFFFFFFF), fontSize: 10);
      final tipPainters = lines
          .map(
            (l) => TextPainter(
              text: TextSpan(text: l, style: tipStyle),
              textDirection: TextDirection.ltr,
            )..layout(),
          )
          .toList();
      final tipW =
          tipPainters.map((t) => t.width).reduce((a, b) => a > b ? a : b) + 12;
      final tipH = tipPainters.fold(0.0, (sum, t) => sum + t.height) + 10;

      var tipX = crossX + 8;
      if (tipX + tipW > size.width) tipX = crossX - tipW - 8;
      final tipY = 4.0;

      final bgPaint = Paint()..color = const Color(0xCC1A1A2E);
      final rrect = RRect.fromRectAndRadius(
        Rect.fromLTWH(tipX, tipY, tipW, tipH),
        const Radius.circular(4),
      );
      canvas.drawRRect(rrect, bgPaint);
      canvas.drawRRect(
        rrect,
        Paint()
          ..color = const Color(0x44FFFFFF)
          ..style = PaintingStyle.stroke
          ..strokeWidth = 0.5,
      );

      var offsetY = tipY + 5;
      for (final tp in tipPainters) {
        tp.paint(canvas, Offset(tipX + 6, offsetY));
        offsetY += tp.height;
      }
    }
    canvas.restore();

    _drawFrameXAxis(
      canvas: canvas,
      size: size,
      axisTop: chartH,
      labelW: labelW,
      frames: frames,
      visibleStart: visibleStart,
      visibleEnd: visibleEnd,
      ptsOrder: ptsOrder,
      xForFrame: (idx) {
        final frac = (idx - viewStart) / span;
        return labelW + frac * chartW + barW / 2;
      },
    );
  }

  static TextPainter _axisLabelPainter(
    List<String> lines,
    TextStyle style,
    double maxWidth,
  ) {
    return TextPainter(
      text: TextSpan(text: lines.join('\n'), style: style),
      textAlign: TextAlign.right,
      textDirection: TextDirection.ltr,
      maxLines: lines.length,
    )..layout(maxWidth: maxWidth);
  }

  static List<String> _byteAxisLabelLines(String text) {
    final parts = text.split(' ');
    if (parts.length == 2 && parts[0] != '0') {
      return [parts[0], parts[1]];
    }
    return [text];
  }

  static String _formatQpLabel(double value) {
    final rounded = value.roundToDouble();
    if ((value - rounded).abs() < 0.05) return '${rounded.toInt()}';
    return value.toStringAsFixed(1);
  }

  static String _formatBytes(int bytes) {
    if (bytes <= 0) return '0 B';
    if (bytes < 1024) return '$bytes B';
    final kb = bytes / 1024;
    if (kb < 1024) return '${kb.toStringAsFixed(1)} KB';
    final mb = kb / 1024;
    return '${mb.toStringAsFixed(1)} MB';
  }

  @override
  bool shouldRepaint(covariant _FrameTrendPainter old) =>
      frames.length != old.frames.length ||
      currentIdx != old.currentIdx ||
      selectedFrameIdx != old.selectedFrameIdx ||
      viewStart != old.viewStart ||
      viewEnd != old.viewEnd ||
      frameSizeAxisZoom != old.frameSizeAxisZoom ||
      qpAxisZoom != old.qpAxisZoom ||
      ptsOrder != old.ptsOrder ||
      hoverX != old.hoverX;
}

// ===========================================================================
// Shared chart scrollbar
// ===========================================================================

class _ChartScrollbar extends StatefulWidget {
  final double total;
  final double viewStart;
  final double viewEnd;
  final ValueChanged<double> onPan;

  const _ChartScrollbar({
    required this.total,
    required this.viewStart,
    required this.viewEnd,
    required this.onPan,
  });

  @override
  State<_ChartScrollbar> createState() => _ChartScrollbarState();
}

class _ChartScrollbarState extends State<_ChartScrollbar> {
  double _dragStart = 0;
  double _dragOffsetStart = 0;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final trackH = 8.0;
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onHorizontalDragStart: (details) {
        _dragStart = details.globalPosition.dx;
        _dragOffsetStart = widget.viewStart;
      },
      onHorizontalDragUpdate: (details) {
        final box = context.findRenderObject() as RenderBox;
        final trackW = box.size.width;
        final deltaPx = details.globalPosition.dx - _dragStart;
        final viewSpan = widget.viewEnd - widget.viewStart;
        final maxOffset = (widget.total - viewSpan).clamp(0.0, double.infinity);
        final newOffset = (_dragOffsetStart + deltaPx / trackW * widget.total)
            .clamp(0.0, maxOffset);
        widget.onPan(newOffset);
      },
      child: CustomPaint(
        painter: _ScrollbarPainter(
          total: widget.total,
          viewStart: widget.viewStart,
          viewEnd: widget.viewEnd,
          trackColor: theme.colorScheme.surfaceContainerHighest,
          thumbColor: theme.colorScheme.onSurface.withValues(alpha: 0.3),
          thumbHoverColor: theme.colorScheme.onSurface.withValues(alpha: 0.5),
        ),
        size: Size(Size.infinite.width, trackH),
      ),
    );
  }
}

class _ScrollbarPainter extends CustomPainter {
  final double total;
  final double viewStart;
  final double viewEnd;
  final Color trackColor;
  final Color thumbColor;
  final Color thumbHoverColor;

  _ScrollbarPainter({
    required this.total,
    required this.viewStart,
    required this.viewEnd,
    required this.trackColor,
    required this.thumbColor,
    required this.thumbHoverColor,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (total <= 0) return;
    const minThumbW = 44.0;

    // Track
    canvas.drawRect(
      Rect.fromLTWH(0, size.height - 3, size.width, 3),
      Paint()..color = trackColor,
    );

    // Thumb
    final rawLeft = (viewStart / total) * size.width;
    final rawRight = (viewEnd / total) * size.width;
    final rawW = (rawRight - rawLeft).clamp(0.0, size.width);
    final thumbW = size.width <= minThumbW
        ? size.width
        : rawW.clamp(minThumbW, size.width);
    final center = (rawLeft + rawRight) / 2;
    final left = (center - thumbW / 2).clamp(0.0, size.width - thumbW);
    canvas.drawRRect(
      RRect.fromRectAndRadius(
        Rect.fromLTWH(left, 0, thumbW, size.height),
        const Radius.circular(3),
      ),
      Paint()..color = thumbColor,
    );
  }

  @override
  bool shouldRepaint(covariant _ScrollbarPainter old) =>
      viewStart != old.viewStart ||
      viewEnd != old.viewEnd ||
      total != old.total;
}

// ===========================================================================
// NALU Browser — search bar + scrollable list
// ===========================================================================

class _NaluBrowserView extends StatefulWidget {
  final List<NaluInfo> nalus;
  final AnalysisCodec codec;
  final int? selectedIdx;
  final ValueChanged<int> onSelected;
  final String filter;
  final ValueChanged<String> onFilterChanged;

  const _NaluBrowserView({
    required this.nalus,
    required this.codec,
    required this.selectedIdx,
    required this.onSelected,
    required this.filter,
    required this.onFilterChanged,
  });

  @override
  State<_NaluBrowserView> createState() => _NaluBrowserViewState();
}

class _NaluBrowserViewState extends State<_NaluBrowserView> {
  static const _itemExtent = 28.0;
  final _scrollController = ScrollController();

  @override
  void initState() {
    super.initState();
    _scheduleScrollSelectedIntoView();
  }

  @override
  void didUpdateWidget(covariant _NaluBrowserView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.selectedIdx != oldWidget.selectedIdx ||
        widget.filter != oldWidget.filter ||
        widget.nalus.length != oldWidget.nalus.length) {
      _scheduleScrollSelectedIntoView();
    }
  }

  @override
  void dispose() {
    _scrollController.dispose();
    super.dispose();
  }

  List<int> _visibleIndices() {
    final filter = widget.filter.toLowerCase();
    return [
      for (var i = 0; i < widget.nalus.length; i++)
        if (filter.isEmpty ||
            bitstreamUnitTypeName(
              widget.codec,
              widget.nalus[i].nalType,
            ).toLowerCase().contains(filter) ||
            '#$i'.contains(filter) ||
            '${widget.nalus[i].nalType}'.contains(filter))
          i,
    ];
  }

  void _scheduleScrollSelectedIntoView() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || !_scrollController.hasClients) return;
      final selectedIdx = widget.selectedIdx;
      if (selectedIdx == null) return;
      final displayIndex = _visibleIndices().indexOf(selectedIdx);
      if (displayIndex < 0) return;

      final position = _scrollController.position;
      final itemTop = displayIndex * _itemExtent;
      final itemBottom = itemTop + _itemExtent;
      final viewportTop = position.pixels;
      final viewportBottom = viewportTop + position.viewportDimension;

      double? target;
      if (itemTop < viewportTop) {
        target = itemTop;
      } else if (itemBottom > viewportBottom) {
        target = itemBottom - position.viewportDimension;
      }
      if (target == null) return;

      _scrollController.jumpTo(
        target
            .clamp(position.minScrollExtent, position.maxScrollExtent)
            .toDouble(),
      );
    });
  }

  @override
  Widget build(BuildContext context) {
    if (widget.nalus.isEmpty) {
      return Center(
        child: Text(AppLocalizations.of(context)!.analysisNoNaluData),
      );
    }
    final theme = Theme.of(context);
    final filter = widget.filter.toLowerCase();
    final visible = _visibleIndices();

    return Column(
      children: [
        // Search bar — same height as list items (28px)
        Padding(
          padding: const EdgeInsets.all(4),
          child: TextField(
            onChanged: widget.onFilterChanged,
            style: theme.textTheme.bodySmall,
            decoration: InputDecoration(
              hintText: AppLocalizations.of(context)!.analysisFilterHint,
              hintStyle: theme.textTheme.bodySmall?.copyWith(
                color: theme.colorScheme.onSurfaceVariant.withValues(
                  alpha: 0.5,
                ),
              ),
              prefixIcon: Icon(
                Icons.search,
                size: 14,
                color: theme.colorScheme.onSurfaceVariant,
              ),
              prefixIconConstraints: const BoxConstraints(
                minWidth: 20,
                minHeight: 0,
              ),
              isDense: true,
              contentPadding: const EdgeInsets.symmetric(
                horizontal: 4,
                vertical: 6,
              ),
              border: OutlineInputBorder(
                borderRadius: BorderRadius.circular(4),
                borderSide: BorderSide(color: theme.colorScheme.outlineVariant),
              ),
              enabledBorder: OutlineInputBorder(
                borderRadius: BorderRadius.circular(4),
                borderSide: BorderSide(color: theme.colorScheme.outlineVariant),
              ),
            ),
          ),
        ),
        // Result count
        if (filter.isNotEmpty)
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 4),
            child: Align(
              alignment: Alignment.centerLeft,
              child: Text(
                '${visible.length} / ${widget.nalus.length}',
                style: theme.textTheme.labelSmall?.copyWith(
                  color: theme.colorScheme.onSurfaceVariant,
                ),
              ),
            ),
          ),
        // List
        Expanded(
          child: ExcludeSemantics(
            child: ListView.builder(
              controller: _scrollController,
              itemCount: visible.length,
              itemExtent: _itemExtent,
              itemBuilder: (context, displayIndex) {
                final origIdx = visible[displayIndex];
                final n = widget.nalus[origIdx];
                final selected = origIdx == widget.selectedIdx;
                return InkWell(
                  onTap: () => widget.onSelected(origIdx),
                  child: Container(
                    color: selected
                        ? theme.colorScheme.primary.withValues(alpha: 0.15)
                        : null,
                    child: Row(
                      children: [
                        // Decorative color bar
                        Container(
                          width: 4,
                          height: 28,
                          color: bitstreamUnitDecorColor(
                            widget.codec,
                            n.nalType,
                            flags: n.flags,
                          ),
                        ),
                        const SizedBox(width: 8),
                        // Index
                        SizedBox(
                          width: 40,
                          child: Text(
                            '#$origIdx',
                            style: theme.textTheme.bodySmall?.copyWith(
                              color: theme.colorScheme.onSurfaceVariant,
                              fontFeatures: [
                                const FontFeature.tabularFigures(),
                              ],
                            ),
                          ),
                        ),
                        const SizedBox(width: 6),
                        // Type number
                        SizedBox(
                          width: 24,
                          child: Text(
                            '${n.nalType}',
                            style: theme.textTheme.bodySmall?.copyWith(
                              color: theme.colorScheme.onSurfaceVariant,
                              fontFeatures: [
                                const FontFeature.tabularFigures(),
                              ],
                            ),
                          ),
                        ),
                        const SizedBox(width: 6),
                        // Type name
                        Expanded(
                          child: Text(
                            bitstreamUnitTypeName(widget.codec, n.nalType),
                            style: theme.textTheme.bodySmall,
                          ),
                        ),
                      ],
                    ),
                  ),
                );
              },
            ),
          ),
        ),
      ],
    );
  }
}

// ===========================================================================
// NALU Detail
// ===========================================================================

class _NaluDetailView extends StatelessWidget {
  final NaluInfo? nalu;
  final int? frameIdx;
  final List<FrameInfo> frames;
  final AnalysisCodec codec;
  final AppLocalizations l;

  const _NaluDetailView({
    required this.nalu,
    this.frameIdx,
    required this.frames,
    required this.codec,
    required this.l,
  });

  @override
  Widget build(BuildContext context) {
    if (nalu == null) {
      return Center(child: Text(l.analysisSelectNalu));
    }
    final n = nalu!;
    final theme = Theme.of(context);
    final ts = theme.textTheme.bodySmall!;
    final labelColor = theme.colorScheme.onSurfaceVariant;

    // NALU-level info
    final items = <_DetailRow>[
      _DetailRow(
        l.analysisType,
        '${bitstreamUnitTypeName(codec, n.nalType)} (${n.nalType})',
      ),
      _DetailRow(l.analysisTemporalId, '${n.temporalId}'),
      _DetailRow(l.analysisLayerId, '${n.layerId}'),
      _DetailRow(l.analysisOffset, '${n.offset}'),
      _DetailRow(l.analysisSize, l.analysisBytes(n.size)),
      _DetailRow('VCL', '${(n.flags & 0x01) != 0}'),
      _DetailRow('Slice', '${(n.flags & 0x02) != 0}'),
      _DetailRow('Keyframe', '${(n.flags & 0x04) != 0}'),
    ];

    // Frame-level info from VBS2 (when this NALU corresponds to a frame)
    final frameItems = <_DetailRow>[];
    if (frameIdx != null && frameIdx! >= 0 && frameIdx! < frames.length) {
      final f = frames[frameIdx!];
      final sliceName = _frameSliceName(f);
      final nalName = bitstreamUnitTypeName(codec, f.nalType);

      frameItems.addAll([
        _DetailRow('Slice', '$sliceName (${f.sliceType})'),
        _DetailRow('NAL Unit', '$nalName (${f.nalType})'),
        _DetailRow('POC', '${f.poc}'),
        _DetailRow('Avg QP', '${f.avgQp}'),
        _DetailRow('Temporal ID', '${f.temporalId}'),
        _DetailRow(
          'Ref L0',
          f.numRefL0 > 0 ? f.refPocsL0.take(f.numRefL0).join(', ') : '-',
        ),
        _DetailRow(
          'Ref L1',
          f.numRefL1 > 0 ? f.refPocsL1.take(f.numRefL1).join(', ') : '-',
        ),
        _DetailRow('Pkt Size', l.analysisBytes(f.packetSize)),
        _DetailRow('PTS', '${f.pts}'),
        _DetailRow('DTS', '${f.dts}'),
      ]);
    }

    Widget section(String title, List<_DetailRow> rows) {
      if (rows.isEmpty) return const SizedBox.shrink();
      return Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(title, style: theme.textTheme.titleSmall),
          const SizedBox(height: 4),
          ...rows.map(
            (r) => Padding(
              padding: const EdgeInsets.symmetric(vertical: 1.5),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  SizedBox(
                    width: 80,
                    child: Text(r.label, style: ts.copyWith(color: labelColor)),
                  ),
                  Expanded(child: Text(r.value, style: ts)),
                ],
              ),
            ),
          ),
        ],
      );
    }

    return Padding(
      padding: const EdgeInsets.all(12),
      child: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            section(l.analysisNaluDetail, items),
            if (frameItems.isNotEmpty) ...[
              const SizedBox(height: 12),
              const Divider(height: 1),
              const SizedBox(height: 8),
              section('Frame Info', frameItems),
            ],
          ],
        ),
      ),
    );
  }
}

class _DetailRow {
  final String label;
  final String value;
  const _DetailRow(this.label, this.value);
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
      'ASSERT_ANALYSIS_NALU_NAME' => _AnalysisTestCommand.assertNaluName,
      'ASSERT_ANALYSIS_SELECTED_FRAME' =>
        _AnalysisTestCommand.assertSelectedFrame,
      'ASSERT_ANALYSIS_SELECTED_FRAME_VISIBLE' =>
        _AnalysisTestCommand.assertSelectedFrameVisible,
      'SET_ANALYSIS_TAB' => _AnalysisTestCommand.setTab,
      'ASSERT_ANALYSIS_TAB' => _AnalysisTestCommand.assertTab,
      'SET_ANALYSIS_ORDER' => _AnalysisTestCommand.setOrder,
      'ASSERT_ANALYSIS_ORDER' => _AnalysisTestCommand.assertOrder,
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

String _frameSliceName(FrameInfo f) => switch (f.sliceType) {
  2 => 'I',
  1 => 'P',
  _ => f.numRefL1 > 0 ? 'B' : 'B (uni)',
};

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

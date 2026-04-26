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
          : _AnalysisTrackPane(
              entry: entries[selected],
              index: selected,
              selected: true,
              showModeToggle: true,
              splitView: _splitView,
              modeToggleEnabled: modeToggleEnabled,
              onModeChanged: (value) => setState(() => _splitView = value),
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
                  Expanded(child: _splitCell(context, row * columns + col)),
              ],
            ),
          ),
      ],
    );
  }

  Widget _splitCell(BuildContext context, int index) {
    if (index >= entries.length) return const SizedBox.shrink();
    final entry = entries[index];
    final theme = Theme.of(context);
    return Container(
      decoration: BoxDecoration(
        border: Border.all(color: theme.colorScheme.outlineVariant),
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
          height: 42,
          padding: const EdgeInsets.symmetric(horizontal: 8),
          decoration: BoxDecoration(
            color: selected
                ? theme.colorScheme.primaryContainer.withValues(alpha: 0.18)
                : theme.colorScheme.surface.withValues(alpha: 0.18),
            border: Border(bottom: BorderSide(color: theme.dividerColor)),
          ),
          child: Row(
            children: [
              Visibility(
                visible: showModeToggle,
                maintainAnimation: true,
                maintainSize: true,
                maintainState: true,
                child: _AnalysisWorkspaceModeToggle(
                  splitView: splitView,
                  enabled: modeToggleEnabled,
                  onChanged: onModeChanged,
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: InkWell(
                  onTap: onSelected,
                  child: Align(
                    alignment: Alignment.centerLeft,
                    child: Text(
                      '${index + 1}. ${entry.fileName ?? 'Track ${index + 1}'}',
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      softWrap: false,
                      style: theme.textTheme.titleSmall?.copyWith(
                        color: theme.colorScheme.onSurface,
                        fontWeight: selected ? FontWeight.w600 : null,
                      ),
                    ),
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
    return ViewModeSelector(
      currentMode: splitView ? 1 : 0,
      onChanged: (value) => onChanged(value == 1),
      firstLabel: l.analysisTabsMode,
      secondLabel: l.analysisSplitMode,
      width: 124,
      height: 30,
      enabled: enabled,
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
  NakiAnalysisSummary? _summary;
  Timer? _pollTimer;
  bool _testStarted = false;

  // Precomputed mappings rebuilt when data loads
  Map<int, List<int>> _pocToIndices = {};
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
            _selectedNaluIdx = idx;
            _selectedFrameIdx = _naluToFrameIdx(idx);
          });
        } else {
          _selectedNaluIdx = idx;
          _selectedFrameIdx = _naluToFrameIdx(idx);
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
    // POC → indices map
    _pocToIndices = <int, List<int>>{};
    for (var i = 0; i < _frames.length; i++) {
      (_pocToIndices[_frames[i].poc] ??= []).add(i);
    }

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

  void _rebuildSortedFramesCache() {
    _sortedFramesCache = List<FrameInfo>.from(_frames);
    if (_ptsOrder) {
      _sortedFramesCache.sort((a, b) => a.pts.compareTo(b.pts));
    }
    // else: keep original order (= DTS / decode order from C++)
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    return Scaffold(
      body: Column(
        children: [
          // Top bar: order toggle + tab bar
          Container(
            height: 40,
            padding: const EdgeInsets.symmetric(horizontal: 8),
            decoration: BoxDecoration(
              border: Border(bottom: BorderSide(color: theme.dividerColor)),
            ),
            child: Row(
              children: [
                // Order toggle
                _OrderToggle(
                  ptsOrder: _ptsOrder,
                  onChanged: (v) => setState(() {
                    _ptsOrder = v;
                    _rebuildSortedFramesCache();
                  }),
                  l: l,
                ),
                const Spacer(),
                // Tab bar
                _TabBar(
                  selectedTab: _selectedTab,
                  onTabChanged: (i) => setState(() => _selectedTab = i),
                  l: l,
                ),
              ],
            ),
          ),
          // Top panel: ref pyramid or frame trend (40%)
          Expanded(
            flex: 4,
            child: _selectedTab == 0
                ? _ReferencePyramidView(
                    frames: _sortedFrames,
                    currentIdx: _summary?.currentFrameIdx ?? -1,
                    selectedFrameIdx: _selectedFrameIdx,
                    pocToIndices: _pocToIndices,
                    onFrameSelected: (i) => setState(() {
                      _selectedFrameIdx = i;
                      _selectedNaluIdx = i != null ? _frameToNaluIdx(i) : null;
                    }),
                    viewStart: _chartOffset,
                    viewEnd: _chartOffset + _visibleFrameCount,
                    onZoom: _chartZoom,
                    onPan: _chartPan,
                    l: l,
                  )
                : _FrameTrendView(
                    frames: _sortedFrames,
                    currentIdx: _summary?.currentFrameIdx ?? -1,
                    selectedFrameIdx: _selectedFrameIdx,
                    viewStart: _chartOffset,
                    viewEnd: _chartOffset + _visibleFrameCount,
                    onZoom: _chartZoom,
                    onPan: _chartPan,
                    onFrameSelected: (i) => setState(() {
                      _selectedFrameIdx = i;
                      _selectedNaluIdx = i != null ? _frameToNaluIdx(i) : null;
                    }),
                    l: l,
                  ),
          ),
          const Divider(height: 1),
          // Bottom: NALU browser + detail (60%) — draggable splitter
          Expanded(
            flex: 6,
            child: LayoutBuilder(
              builder: (context, constraints) {
                final totalW = constraints.maxWidth;
                final maxBrowserW =
                    totalW - 120; // leave at least 120px for detail
                final browserW = _naluBrowserWidth.clamp(120.0, maxBrowserW);
                return Stack(
                  children: [
                    Row(
                      children: [
                        // NALU browser
                        SizedBox(
                          width: browserW,
                          child: _NaluBrowserView(
                            nalus: _nalus,
                            selectedIdx: _selectedNaluIdx,
                            onSelected: (i) => setState(() {
                              _selectedNaluIdx = i;
                              _selectedFrameIdx = _naluToFrameIdx(i);
                            }),
                            filter: _naluFilter,
                            onFilterChanged: (v) =>
                                setState(() => _naluFilter = v),
                          ),
                        ),
                        // Visual-only divider line
                        Container(
                          width: 1,
                          color: theme.colorScheme.outlineVariant,
                        ),
                        // NALU detail
                        Expanded(
                          child: _NaluDetailView(
                            nalu:
                                _selectedNaluIdx != null &&
                                    _selectedNaluIdx! < _nalus.length
                                ? _nalus[_selectedNaluIdx!]
                                : null,
                            frameIdx: _selectedFrameIdx,
                            frames: _frames,
                            l: l,
                          ),
                        ),
                      ],
                    ),
                    // Draggable splitter hit area
                    Positioned(
                      left: browserW - 4,
                      top: 0,
                      bottom: 0,
                      width: 9,
                      child: _ResizableVDivider(
                        position: browserW,
                        onPositionChanged: (v) =>
                            setState(() => _naluBrowserWidth = v),
                      ),
                    ),
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
    return SegmentedButton<bool>(
      showSelectedIcon: false,
      segments: [
        ButtonSegment(value: true, label: Text(l.analysisPtsOrder)),
        ButtonSegment(value: false, label: Text(l.analysisDtsOrder)),
      ],
      selected: {ptsOrder},
      onSelectionChanged: (s) => onChanged(s.first),
      style: const ButtonStyle(
        visualDensity: VisualDensity.compact,
        textStyle: WidgetStatePropertyAll(TextStyle(fontSize: 12)),
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
    return SegmentedButton<int>(
      showSelectedIcon: false,
      segments: [
        ButtonSegment(
          value: 0,
          label: _StableSegmentLabel(l.analysisRefPyramid),
        ),
        ButtonSegment(
          value: 1,
          label: _StableSegmentLabel(l.analysisFrameTrend),
        ),
      ],
      selected: {selectedTab},
      onSelectionChanged: (s) => onTabChanged(s.first),
      style: const ButtonStyle(
        visualDensity: VisualDensity.compact,
        textStyle: WidgetStatePropertyAll(TextStyle(fontSize: 12)),
      ),
    );
  }
}

class _StableSegmentLabel extends StatelessWidget {
  final String text;

  const _StableSegmentLabel(this.text);

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 128,
      child: Center(
        child: Text(
          text,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          softWrap: false,
        ),
      ),
    );
  }
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

// ===========================================================================
// Reference Pyramid — circle nodes + reference arrows + level backgrounds
// Supports zoom (scroll wheel) and pan (scrollbar).
// ===========================================================================

class _ReferencePyramidView extends StatefulWidget {
  final List<FrameInfo> frames;
  final int currentIdx;
  final int? selectedFrameIdx;
  final Map<int, List<int>> pocToIndices;
  final ValueChanged<int?> onFrameSelected;
  final double viewStart;
  final double viewEnd;
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
    required this.onZoom,
    required this.onPan,
    required this.l,
  });

  @override
  State<_ReferencePyramidView> createState() => _ReferencePyramidViewState();
}

class _ReferencePyramidViewState extends State<_ReferencePyramidView> {
  _RefPyramidPainter? _lastPainter;

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
              child: CustomPaint(
                painter: _lastPainter = _RefPyramidPainter(
                  frames: widget.frames,
                  currentIdx: widget.currentIdx,
                  selectedFrameIdx: widget.selectedFrameIdx,
                  pocToIndices: widget.pocToIndices,
                  viewStart: widget.viewStart,
                  viewEnd: widget.viewEnd,
                ),
                size: Size.infinite,
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

  /// Populated during paint() — used by parent for hit-testing.
  List<(int, Rect)> frameRects = [];

  _RefPyramidPainter({
    required this.frames,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.pocToIndices,
    required this.viewStart,
    required this.viewEnd,
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
    final numLevels = maxTid + 1;
    final rowH = size.height / numLevels;
    final labelW = 36.0;
    final usableW = size.width - labelW;
    final span = viewEnd - viewStart;
    final circleR = (rowH * 0.3).clamp(6.0, 20.0);

    final positions = <int, Offset>{}; // globalIdx → position
    for (var i = visibleStart; i < visibleEnd; i++) {
      final frac = (i - viewStart) / span;
      final x = labelW + frac * usableW;
      final y = size.height - (frames[i].temporalId + 0.5) * rowH;
      positions[i] = Offset(x, y);
    }
    frameRects = [
      for (final e in positions.entries)
        (e.key, Rect.fromCircle(center: e.value, radius: circleR)),
    ];

    // --- Level backgrounds ---
    for (var tid = 0; tid <= maxTid; tid++) {
      final top = size.height - (tid + 1) * rowH;
      final alpha = 0.03 + (maxTid - tid) * 0.025;
      _bgPaint.color = const Color(0xFFFFFFFF).withValues(alpha: alpha);
      canvas.drawRect(Rect.fromLTWH(0, top, size.width, rowH), _bgPaint);
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
      tp.paint(canvas, Offset(4, top + rowH / 2 - tp.height / 2));
    }
    // --- Current playback cursor ---
    if (currentIdx >= 0 && positions.containsKey(currentIdx)) {
      final cx = positions[currentIdx]!.dx;
      _cursorPaint.color = const Color(0xFFFFFFFF).withValues(alpha: 0.15);
      _cursorPaint.strokeWidth = 1;
      canvas.drawLine(Offset(cx, 0), Offset(cx, size.height), _cursorPaint);
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
      final x = labelW + frac * usableW;
      final y = size.height - (frames[idx].temporalId + 0.5) * rowH;
      return Offset(x, y);
    }

    // Collect selected frame's references for highlighting
    final Set<int> selectedRefs = {};
    if (selectedFrameIdx != null &&
        selectedFrameIdx! < frames.length &&
        positions.containsKey(selectedFrameIdx)) {
      final sf = frames[selectedFrameIdx!];
      for (var j = 0; j < sf.numRefL0 && j < sf.refPocsL0.length; j++) {
        final ri = nearestRefIdx(sf.refPocsL0[j], selectedFrameIdx!);
        if (ri != null && positions.containsKey(ri)) selectedRefs.add(ri);
      }
      for (var j = 0; j < sf.numRefL1 && j < sf.refPocsL1.length; j++) {
        final ri = nearestRefIdx(sf.refPocsL1[j], selectedFrameIdx!);
        if (ri != null && positions.containsKey(ri)) selectedRefs.add(ri);
      }
    }

    // Helper: get fill color for a frame's circle (same logic as circle drawing)
    Color frameFillColor(FrameInfo f) {
      if (f.sliceType == 2) return const Color(0xFFFF4D4F); // I: red
      if (f.sliceType == 0 && f.numRefL1 > 0) {
        return const Color(0xFF1890FF); // B bidir: blue
      }
      return const Color(0xFF52C41A); // B uni / P: green
    }

    // Draw all visible arrows
    for (var i = visibleStart; i < visibleEnd; i++) {
      if (!positions.containsKey(i)) continue;
      final f = frames[i];
      if (f.numRefL0 == 0 && f.numRefL1 == 0) continue;
      final from = positions[i]!;

      final isSelLine =
          selectedFrameIdx != null &&
          (i == selectedFrameIdx || selectedRefs.contains(i));
      final lineW = isSelLine ? 2.5 : 1.0;
      final arrowAlpha = isSelLine
          ? 1.0
          : (selectedFrameIdx != null ? 0.2 : 0.5);

      for (var j = 0; j < f.numRefL0 && j < f.refPocsL0.length; j++) {
        final ri = nearestRefIdx(f.refPocsL0[j], i);
        if (ri != null && ri < frames.length) {
          final arrowColor = frameFillColor(
            frames[ri],
          ).withValues(alpha: arrowAlpha);
          _drawArrow(canvas, from, posFor(ri), arrowColor, lineW, circleR);
        }
      }
      for (var j = 0; j < f.numRefL1 && j < f.refPocsL1.length; j++) {
        final ri = nearestRefIdx(f.refPocsL1[j], i);
        if (ri != null && ri < frames.length) {
          final arrowColor = frameFillColor(
            frames[ri],
          ).withValues(alpha: arrowAlpha);
          _drawArrow(canvas, from, posFor(ri), arrowColor, lineW, circleR);
        }
      }
    }
    // --- Frame circles ---
    final related = <int>{};
    if (selectedFrameIdx != null) {
      related.add(selectedFrameIdx!);
      related.addAll(selectedRefs);
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

  @override
  bool shouldRepaint(covariant _RefPyramidPainter old) =>
      frames.length != old.frames.length ||
      currentIdx != old.currentIdx ||
      selectedFrameIdx != old.selectedFrameIdx ||
      viewStart != old.viewStart ||
      viewEnd != old.viewEnd;
}

// ===========================================================================
// Frame Trend — zoomable / pannable bar chart
// ===========================================================================

class _FrameTrendView extends StatefulWidget {
  final List<FrameInfo> frames;
  final int currentIdx;
  final int? selectedFrameIdx;
  final double viewStart;
  final double viewEnd;
  final ValueChanged<double> onZoom;
  final ValueChanged<double> onPan;
  final ValueChanged<int?> onFrameSelected;
  final AppLocalizations l;
  const _FrameTrendView({
    required this.frames,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.viewStart,
    required this.viewEnd,
    required this.onZoom,
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
          child: MouseRegion(
            onExit: (_) => setState(() => _hoverX = null),
            child: Listener(
              onPointerSignal: (signal) {
                if (signal is PointerScrollEvent) {
                  w.onZoom(signal.scrollDelta.dy);
                }
              },
              child: GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTapUp: (details) {
                  final labelW = 36.0;
                  final box = context.findRenderObject() as RenderBox;
                  final localX = box.globalToLocal(details.globalPosition).dx;
                  final chartW = box.size.width - labelW;
                  if (chartW <= 0 || localX < labelW) {
                    w.onFrameSelected(null);
                    return;
                  }
                  final span = w.viewEnd - w.viewStart;
                  final idx =
                      (w.viewStart + ((localX - labelW) / chartW) * span)
                          .round()
                          .clamp(0, w.frames.length - 1);
                  w.onFrameSelected(w.selectedFrameIdx == idx ? null : idx);
                },
                onPanUpdate: (details) {
                  final box = context.findRenderObject() as RenderBox;
                  final local = box.globalToLocal(details.globalPosition);
                  setState(() => _hoverX = local.dx);
                },
                child: MouseRegion(
                  onHover: (e) {
                    final box = context.findRenderObject() as RenderBox;
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
                      hoverX: _hoverX,
                    ),
                    size: Size.infinite,
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
  final double? hoverX;

  _FrameTrendPainter({
    required this.frames,
    required this.currentIdx,
    required this.selectedFrameIdx,
    required this.viewStart,
    required this.viewEnd,
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

    // Layout: same labelW as pyramid (36px) left column
    final labelW = 36.0;
    final chartW = size.width - labelW;
    final upperH = size.height * 0.58;
    final lowerH = size.height * 0.32;
    final gapH = size.height * 0.05;
    final lowerTop = upperH + gapH;

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
    final qpLow = (minQp / 5).floor() * 5;
    final qpHigh = ((maxQp / 5).floor() + 1) * 5;
    final qpRange = (qpHigh - qpLow).clamp(5, 63);

    // Label style — same as pyramid level labels
    const labelStyle = TextStyle(
      color: Color(0xFFFFFFFF),
      fontSize: 10,
      fontWeight: FontWeight.w600,
      letterSpacing: 0.5,
    );
    final gridPaint = Paint()
      ..color = const Color(0xFFFFFFFF).withValues(alpha: 0.06)
      ..strokeWidth = 0.5;

    // --- Packet size axis labels (upper) ---
    // Only bottom (0) and top (max) to keep it minimal like pyramid
    final sizeLabels = [
      (0.0, _formatBytes(0)),
      (0.5, _formatBytes((maxPacketSize * 0.5).round())),
      (1.0, _formatBytes(maxPacketSize)),
    ];
    for (final (yFrac, text) in sizeLabels) {
      final y = upperH * (1 - yFrac);
      canvas.drawLine(Offset(labelW, y), Offset(size.width, y), gridPaint);
      final tp = TextPainter(
        text: TextSpan(text: text, style: labelStyle),
        textDirection: TextDirection.ltr,
      )..layout();
      // Clamp to avoid clipping at top
      final drawY = (y - tp.height / 2).clamp(0.0, upperH - tp.height);
      tp.paint(canvas, Offset(4, drawY));
    }

    // --- QP axis labels (lower) ---
    final qpLabels = [
      (0.0, qpLow),
      (0.5, (qpLow + qpHigh) ~/ 2),
      (1.0, qpHigh),
    ];
    for (final (yFrac, value) in qpLabels) {
      final y = lowerTop + lowerH * (1 - yFrac);
      canvas.drawLine(Offset(labelW, y), Offset(size.width, y), gridPaint);
      final tp = TextPainter(
        text: TextSpan(text: '$value', style: labelStyle),
        textDirection: TextDirection.ltr,
      )..layout();
      final drawY = (y - tp.height / 2).clamp(
        lowerTop,
        lowerTop + lowerH - tp.height,
      );
      tp.paint(canvas, Offset(4, drawY));
    }

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
      final h = (f.packetSize / maxPacketSize) * upperH;

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
      final y = lowerTop + lowerH * (1 - (f.avgQp - qpLow) / qpRange);
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
        Offset(cx, size.height),
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
        Offset(crossX, size.height),
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

    // Track
    canvas.drawRect(
      Rect.fromLTWH(0, size.height - 3, size.width, 3),
      Paint()..color = trackColor,
    );

    // Thumb
    final left = (viewStart / total) * size.width;
    final right = (viewEnd / total) * size.width;
    canvas.drawRRect(
      RRect.fromRectAndRadius(
        Rect.fromLTWH(left, 0, right - left, size.height),
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
  final int? selectedIdx;
  final ValueChanged<int> onSelected;
  final String filter;
  final ValueChanged<String> onFilterChanged;

  const _NaluBrowserView({
    required this.nalus,
    required this.selectedIdx,
    required this.onSelected,
    required this.filter,
    required this.onFilterChanged,
  });

  @override
  State<_NaluBrowserView> createState() => _NaluBrowserViewState();
}

class _NaluBrowserViewState extends State<_NaluBrowserView> {
  final _scrollController = ScrollController();

  @override
  void dispose() {
    _scrollController.dispose();
    super.dispose();
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

    // Build filtered index map: visible[i] = original index
    final visible = <int>[];
    for (var i = 0; i < widget.nalus.length; i++) {
      if (filter.isEmpty) {
        visible.add(i);
      } else {
        final n = widget.nalus[i];
        final name = h266NaluTypeName(n.nalType).toLowerCase();
        final idStr = '#$i';
        if (name.contains(filter) ||
            idStr.contains(filter) ||
            '${n.nalType}'.contains(filter)) {
          visible.add(i);
        }
      }
    }

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
          child: ListView.builder(
            controller: _scrollController,
            itemCount: visible.length,
            itemExtent: 28,
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
                        color: naluTypeDecorColor(n.nalType),
                      ),
                      const SizedBox(width: 8),
                      // Index
                      SizedBox(
                        width: 40,
                        child: Text(
                          '#$origIdx',
                          style: theme.textTheme.bodySmall?.copyWith(
                            color: theme.colorScheme.onSurfaceVariant,
                            fontFeatures: [const FontFeature.tabularFigures()],
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
                            fontFeatures: [const FontFeature.tabularFigures()],
                          ),
                        ),
                      ),
                      const SizedBox(width: 6),
                      // Type name
                      Expanded(
                        child: Text(
                          h266NaluTypeName(n.nalType),
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
  final AppLocalizations l;

  const _NaluDetailView({
    required this.nalu,
    this.frameIdx,
    required this.frames,
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
        '${h266NaluTypeName(n.nalType)} (${n.nalType})',
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
      final sliceName = switch (f.sliceType) {
        2 => 'I',
        1 => 'P',
        _ => f.numRefL1 > 0 ? 'B' : 'B (uni)', // unidirectional B
      };
      final nalName =
          {
            0: 'TRAIL',
            1: 'STSA',
            2: 'RADL',
            7: 'IDR_W_RADL',
            8: 'IDR_N_LP',
            20: 'AUD',
          }[f.nalType] ??
          '${f.nalType}';

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

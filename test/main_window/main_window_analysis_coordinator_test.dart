import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_manager.dart';
import 'package:void_player/analysis/analysis_overlay.dart';
import 'package:void_player/main_window/main_window_analysis.dart';
import 'package:void_player/main_window/main_window_state.dart';
import 'package:void_player/native_player/native_player_protocol.dart';
import 'package:void_player/track_manager.dart';

class _CountingAnalysisGenerationService extends ChangeNotifier
    implements AnalysisGenerationService {
  int ensureGeneratedCalls = 0;
  int activeEnsureGeneratedCalls = 0;
  int maxActiveEnsureGeneratedCalls = 0;
  int activateOverlayCalls = 0;
  int activateOverlayTracksCalls = 0;
  bool _overlayPanelVisible = false;
  Set<int> _activeOverlayTrackFileIds = const {};
  AnalysisOverlayConfig _config = const AnalysisOverlayConfig();
  int _overlayPresentationRevision = 0;
  Completer<String?>? ensureGeneratedCompleter;
  Duration ensureGeneratedDelay = Duration.zero;
  final List<List<AnalysisOverlayTrackSource>> overlayTrackActivations = [];

  @override
  String? get activeOverlayHash => null;

  @override
  bool get overlayPanelVisible => _overlayPanelVisible;

  @override
  Set<int> get activeOverlayTrackFileIds => _activeOverlayTrackFileIds;

  @override
  AnalysisOverlayConfig get overlayConfig => _config;

  @override
  int get overlayPresentationRevision => _overlayPresentationRevision;

  @override
  Map<String, Object?> nativeOverlayStatePayload() => const {};

  @override
  AnalysisTrackGenerationStatus? statusForPath(String path) => null;

  @override
  bool supportsOverlayForHash(String hash) => true;

  @override
  Future<String?> ensureGenerated(String videoPath) async {
    ensureGeneratedCalls++;
    final call = ensureGeneratedCalls;
    activeEnsureGeneratedCalls++;
    maxActiveEnsureGeneratedCalls =
        activeEnsureGeneratedCalls > maxActiveEnsureGeneratedCalls
        ? activeEnsureGeneratedCalls
        : maxActiveEnsureGeneratedCalls;
    try {
      final completer = ensureGeneratedCompleter;
      if (completer != null) {
        ensureGeneratedCompleter = null;
        return await completer.future;
      }
      if (ensureGeneratedDelay > Duration.zero) {
        await Future<void>.delayed(ensureGeneratedDelay);
      }
      return 'hash-$call';
    } finally {
      activeEnsureGeneratedCalls--;
    }
  }

  @override
  Future<String?> ensureGeneratedAndLoaded(String videoPath) async {
    ensureGeneratedCalls++;
    return 'hash-$ensureGeneratedCalls';
  }

  @override
  Future<bool> ensureOverlayChunk(
    String hash, {
    required String videoPath,
    int? analysisFrameIndex,
    int? sourcePacketIndex,
    int? sourcePacketSize,
    int? sourcePacketPos,
    int? sourcePacketPtsUs,
    int? sourcePacketDtsUs,
    int? presentedPtsUs,
    int? presentedDtsUs,
  }) async {
    return true;
  }

  @override
  Future<bool> activateOverlay(
    String hash, {
    required String name,
    required String path,
    required int trackFileId,
    int? analysisFrameIndex,
    int? sourcePacketIndex,
    int? sourcePacketSize,
    int? sourcePacketPos,
    int? sourcePacketPtsUs,
    int? sourcePacketDtsUs,
    int? presentedPtsUs,
    int? presentedDtsUs,
  }) async {
    activateOverlayCalls++;
    _overlayPanelVisible = true;
    _activeOverlayTrackFileIds = {trackFileId};
    return true;
  }

  @override
  Future<bool> activateOverlayTracks(
    List<AnalysisOverlayTrackSource> tracks,
  ) async {
    activateOverlayTracksCalls++;
    overlayTrackActivations.add(List.unmodifiable(tracks));
    _overlayPanelVisible = tracks.isNotEmpty;
    _activeOverlayTrackFileIds = tracks
        .map((track) => track.trackFileId)
        .toSet();
    return true;
  }

  @override
  void updateOverlayConfig(AnalysisOverlayConfig config) {
    _config = config;
  }

  @override
  void deactivateOverlay() {
    _overlayPanelVisible = false;
    _activeOverlayTrackFileIds = const {};
  }

  void completeOverlayChunkForTest({int trackFileId = 7}) {
    _overlayPanelVisible = true;
    _activeOverlayTrackFileIds = {trackFileId};
    _overlayPresentationRevision++;
    notifyListeners();
  }

  void emitSameOverlayRevisionForTest() {
    notifyListeners();
  }

  void setActiveOverlayTrackForTest(int trackFileId) {
    _overlayPanelVisible = true;
    _activeOverlayTrackFileIds = {trackFileId};
  }
}

TrackManager _trackManagerWithOneTrack() {
  final tracks = TrackManager();
  tracks.addTrack(
    const TrackInfo(
      fileId: 7,
      slot: 0,
      path: '/tmp/video.mp4',
      width: 320,
      height: 180,
      durationUs: 1,
    ),
  );
  return tracks;
}

TrackManager _trackManagerWithTwoTracks() {
  final tracks = _trackManagerWithOneTrack();
  tracks.addTrack(
    const TrackInfo(
      fileId: 8,
      slot: 1,
      path: '/tmp/video-2.mp4',
      width: 320,
      height: 180,
      durationUs: 1,
    ),
  );
  return tracks;
}

void main() {
  test('enterAnalysis generates cache and selects the analysis deck', () async {
    final tracks = _trackManagerWithOneTrack();
    final generation = _CountingAnalysisGenerationService();
    final stateStore = MainWindowStateStore();
    final coordinator = MainWindowAnalysisCoordinator(
      trackManager: tracks,
      stateStore: stateStore,
      analysisGeneration: generation,
    );
    addTearDown(coordinator.dispose);
    addTearDown(tracks.dispose);
    addTearDown(stateStore.dispose);

    await coordinator.enterAnalysis();

    expect(generation.ensureGeneratedCalls, 1);
    expect(stateStore.value.deckTab, MainWindowDeckTab.analysis);
    expect(coordinator.entries.value.single.hash, 'hash-1');
    tracks.clear();
    expect(coordinator.entries.value, isEmpty);
  });

  test(
    'enterAnalysis serializes cache generation for multiple tracks',
    () async {
      final tracks = _trackManagerWithTwoTracks();
      final generation = _CountingAnalysisGenerationService()
        ..ensureGeneratedDelay = const Duration(milliseconds: 1);
      final stateStore = MainWindowStateStore();
      final coordinator = MainWindowAnalysisCoordinator(
        trackManager: tracks,
        stateStore: stateStore,
        analysisGeneration: generation,
      );
      addTearDown(coordinator.dispose);
      addTearDown(tracks.dispose);
      addTearDown(stateStore.dispose);

      await coordinator.enterAnalysis();

      expect(generation.ensureGeneratedCalls, 2);
      expect(generation.maxActiveEnsureGeneratedCalls, 1);
      expect(coordinator.entries.value.map((entry) => entry.hash), [
        'hash-1',
        'hash-2',
      ]);
    },
  );

  test('overlay actions no-op when analysis overlays are disabled', () async {
    final tracks = _trackManagerWithOneTrack();
    final generation = _CountingAnalysisGenerationService();
    final stateStore = MainWindowStateStore();
    final coordinator = MainWindowAnalysisCoordinator(
      trackManager: tracks,
      stateStore: stateStore,
      analysisGeneration: generation,
      analysisOverlaysEnabled: false,
    );
    addTearDown(coordinator.dispose);
    addTearDown(tracks.dispose);
    addTearDown(stateStore.dispose);

    await coordinator.toggleOverlayForSlot(0);
    await coordinator.toggleOverlayPanel();
    await coordinator.refreshOverlayForCurrentFrame();

    expect(generation.ensureGeneratedCalls, 0);
    expect(generation.activateOverlayCalls, 0);
    expect(generation.activateOverlayTracksCalls, 0);
  });

  testWidgets('overlay panel starts low-frequency playback prefetch', (
    tester,
  ) async {
    final tracks = _trackManagerWithOneTrack();
    final generation = _CountingAnalysisGenerationService();
    final stateStore = MainWindowStateStore();
    var redraws = 0;
    var presentedFrameCalls = 0;
    final coordinator = MainWindowAnalysisCoordinator(
      trackManager: tracks,
      stateStore: stateStore,
      analysisGeneration: generation,
      presentedFrameProvider: (fileId) async {
        presentedFrameCalls++;
        return PresentedFrameTiming(
          ptsUs: presentedFrameCalls * 33333,
          dtsUs: presentedFrameCalls * 33333,
        );
      },
      onOverlayStateChanged: () {
        redraws++;
      },
    );
    addTearDown(coordinator.dispose);
    addTearDown(tracks.dispose);
    addTearDown(stateStore.dispose);

    await coordinator.toggleOverlayPanel();
    await tester.pump();

    expect(generation.activateOverlayTracksCalls, 1);
    expect(redraws, 1);
    expect(presentedFrameCalls, 1);

    await tester.pump(const Duration(milliseconds: 750));
    await tester.pump();

    expect(generation.activateOverlayTracksCalls, 2);
    expect(presentedFrameCalls, 2);
    expect(redraws, 1);

    coordinator.deactivateOverlay();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 1500));
    await tester.pump();

    expect(generation.activateOverlayTracksCalls, 2);
  });

  test('overlay chunk readiness requests redraw for paused viewport', () {
    final tracks = _trackManagerWithOneTrack();
    final generation = _CountingAnalysisGenerationService();
    final stateStore = MainWindowStateStore();
    var redraws = 0;
    final coordinator = MainWindowAnalysisCoordinator(
      trackManager: tracks,
      stateStore: stateStore,
      analysisGeneration: generation,
      onOverlayStateChanged: () {
        redraws++;
      },
    );
    addTearDown(coordinator.dispose);
    addTearDown(tracks.dispose);
    addTearDown(stateStore.dispose);

    generation.completeOverlayChunkForTest();

    expect(redraws, 1);

    generation.emitSameOverlayRevisionForTest();

    expect(redraws, 1);
  });

  test(
    'new seek epoch prevents an older overlay refresh from committing',
    () async {
      final tracks = _trackManagerWithOneTrack();
      final generation = _CountingAnalysisGenerationService();
      final stateStore = MainWindowStateStore();
      generation.setActiveOverlayTrackForTest(7);
      final firstHash = Completer<String?>();
      generation.ensureGeneratedCompleter = firstHash;
      var redraws = 0;
      final coordinator = MainWindowAnalysisCoordinator(
        trackManager: tracks,
        stateStore: stateStore,
        analysisGeneration: generation,
        onOverlayStateChanged: () {
          redraws++;
        },
      );
      addTearDown(coordinator.dispose);
      addTearDown(tracks.dispose);
      addTearDown(stateStore.dispose);

      coordinator.beginSeekOverlayRefresh(1);
      final staleRefresh = coordinator.refreshOverlayForPresentedFrame(
        requestId: 1,
        trackFileId: 7,
        ptsUs: 1000000,
        dtsUs: 1000000,
      );
      await Future<void>.delayed(Duration.zero);

      coordinator.beginSeekOverlayRefresh(2);
      final currentRefresh = coordinator.refreshOverlayForPresentedFrame(
        requestId: 2,
        trackFileId: 7,
        ptsUs: 2000000,
        dtsUs: 2000000,
      );
      firstHash.complete('hash-stale');
      await Future.wait([staleRefresh, currentRefresh]);

      expect(generation.activateOverlayTracksCalls, 1);
      expect(
        generation.overlayTrackActivations.single.single.presentedPtsUs,
        2000000,
      );
      expect(redraws, 1);
    },
  );
}

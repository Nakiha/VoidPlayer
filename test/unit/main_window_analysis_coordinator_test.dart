import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_manager.dart';
import 'package:void_player/analysis/analysis_overlay.dart';
import 'package:void_player/native_player/native_player_protocol.dart';
import 'package:void_player/platform/analysis_process_host.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/windows/main/main_window_analysis.dart';

class _CountingAnalysisGenerationService implements AnalysisGenerationService {
  int ensureGeneratedCalls = 0;
  int activateOverlayCalls = 0;
  int activateOverlayTracksCalls = 0;
  AnalysisOverlayConfig _config = const AnalysisOverlayConfig();

  @override
  String? get activeOverlayHash => null;

  @override
  bool get overlayPanelVisible => false;

  @override
  Set<int> get activeOverlayTrackFileIds => const {};

  @override
  AnalysisOverlayConfig get overlayConfig => _config;

  @override
  AnalysisTrackGenerationStatus? statusForPath(String path) => null;

  @override
  bool supportsOverlayForHash(String hash) => true;

  @override
  Future<String?> ensureGenerated(String videoPath) async {
    ensureGeneratedCalls++;
    return 'hash-$ensureGeneratedCalls';
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
    int? presentedPtsUs,
    int? presentedDtsUs,
  }) async {
    activateOverlayCalls++;
    return true;
  }

  @override
  Future<bool> activateOverlayTracks(
    List<AnalysisOverlayTrackSource> tracks,
  ) async {
    activateOverlayTracksCalls++;
    return true;
  }

  @override
  void updateOverlayConfig(AnalysisOverlayConfig config) {
    _config = config;
  }

  @override
  void deactivateOverlay() {}
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

void main() {
  test(
    'triggerAnalysis no-ops when external analysis windows are unsupported',
    () async {
      final tracks = _trackManagerWithOneTrack();
      final generation = _CountingAnalysisGenerationService();
      final coordinator = MainWindowAnalysisCoordinator(
        trackManager: tracks,
        analysisProcesses: UnsupportedAnalysisProcessHost(),
        analysisGeneration: generation,
      );
      addTearDown(coordinator.dispose);
      addTearDown(tracks.dispose);

      await coordinator.triggerAnalysis();

      expect(generation.ensureGeneratedCalls, 0);
    },
  );

  test('overlay actions no-op when analysis overlays are disabled', () async {
    final tracks = _trackManagerWithOneTrack();
    final generation = _CountingAnalysisGenerationService();
    final coordinator = MainWindowAnalysisCoordinator(
      trackManager: tracks,
      analysisProcesses: UnsupportedAnalysisProcessHost(),
      analysisGeneration: generation,
      analysisOverlaysEnabled: false,
    );
    addTearDown(coordinator.dispose);
    addTearDown(tracks.dispose);

    await coordinator.toggleOverlayForSlot(0);
    await coordinator.toggleOverlayPanel();
    await coordinator.refreshOverlayForCurrentFrame();

    expect(generation.ensureGeneratedCalls, 0);
    expect(generation.activateOverlayCalls, 0);
    expect(generation.activateOverlayTracksCalls, 0);
  });
}

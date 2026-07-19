import 'package:flutter/foundation.dart';

import '../analysis/analysis_quality_service.dart';
import '../analysis/analysis_toolbar_data_source.dart';
import '../analysis/ui/analysis_ui_selection.dart';
import '../analysis/ui/testing/analysis_test_host.dart';
import '../analysis/ui/workspace/analysis_workspace_models.dart';
import '../marks/quick_mark.dart';
import '../marks/quick_mark_store.dart';
import '../marks/quick_mark_thumbnail.dart';
import '../platform/platform_capabilities.dart';
import '../preferences/playback_preferences.dart';
import '../session/playback_session.dart';
import '../track_manager.dart';
import '../video_renderer_controller.dart';
import '../viewport/display_geometry.dart';
import '../viewport/viewport_display_state.dart';
import 'main_window_selection.dart';
import 'main_window_state.dart';
import 'main_window_view_model.dart';

class MainWindowViewModelFactory {
  const MainWindowViewModelFactory._();

  static MainWindowViewModel build({
    required PlaybackSession session,
    required LayoutState layout,
    required bool hasPlayer,
    required int? textureId,
    bool nativeCompositorActive = false,
    required ViewportDisplayState viewportState,
    required List<TrackEntry> tracks,
    required QuickMarkView markView,
    required QuickMark? quickMarkDraft,
    required Map<int, QuickMarkThumbnail> quickMarkThumbnails,
    required int currentPtsUs,
    required PlatformCapabilities platformCapabilities,
    required Map<int, int> syncOffsets,
    required int? audibleTrackFileId,
    required PerformanceAlertPolicy performanceAlertPolicy,
    required AnalysisToolbarDataSource analysisDataSource,
    required double timelineStartWidth,
    required bool isPlaying,
    required int durationUs,
    required List<int> markerUs,
    required int? seekMinUs,
    required int? seekMaxUs,
    required bool loopRangeEnabled,
    required int loopStartUs,
    required int loopEndUs,
    required double controlsWidth,
    required bool dragging,
    required bool mediaInfoVisible,
    required bool profilerVisible,
    required bool settingsVisible,
    required bool analysisOverlayControlsVisible,
    required MainWindowDeckTab deckTab,
    required double deckHeight,
    required bool deckCollapsed,
    required ValueListenable<List<AnalysisWorkspaceEntry>> analysisEntries,
    required AnalysisTestHostRegistry analysisTestHosts,
    required AnalysisQualityDataSource qualityDataSource,
    required AnalysisUiSelection? analysisSelection,
    required int? selectedTrackFileId,
    required Map<int, QuickMarkAnchor> presentedFrameAnchors,
    required bool marksSidebarVisible,
    required double marksSidebarWidth,
    required bool fullScreen,
    required bool fullScreenControlsVisible,
  }) {
    QuickMark? selectedMark;
    TrackInfo? selectedTrack;
    for (final mark in markView.allMarks) {
      if (mark.id == markView.selectedMarkId) {
        selectedMark = mark;
        break;
      }
    }
    for (final entry in tracks) {
      if (entry.fileId == selectedTrackFileId) {
        selectedTrack = entry.info;
        break;
      }
    }
    final selection = selectedMark != null
        ? MainWindowQuickMarkSelection(selectedMark)
        : analysisSelection != null
        ? MainWindowAnalysisSelection(analysisSelection)
        : selectedTrack != null
        ? MainWindowTrackSelection(selectedTrack)
        : const MainWindowNoSelection();
    return MainWindowViewModel(
      session: MainWindowSessionVm.fromSession(session),
      viewport: MainWindowViewportVm(
        viewMode: layout.mode,
        viewModeEnabled: hasPlayer,
        textureId: textureId,
        nativeCompositorActive: nativeCompositorActive,
        viewportState: viewportState,
        layout: layout,
        tracks: tracks
            .map((entry) => DisplayTrackGeometry.fromTrackInfo(entry.info))
            .toList(),
        quickMarks: markView.visibleMarks,
        quickMarkDraft: quickMarkDraft,
        selectedQuickMarkId: markView.visibleSelectedMarkId,
      ),
      marks: MainWindowMarksVm(
        allMarks: markView.allMarks,
        visibleMarks: markView.visibleMarks,
        visibleMarkIds: markView.visibleMarkIds,
        selectedMarkId: markView.selectedMarkId,
        tracksByFileId: {for (final entry in tracks) entry.fileId: entry.info},
        thumbnailsByMarkId: quickMarkThumbnails,
        currentPtsUs: currentPtsUs,
      ),
      media: MainWindowMediaVm(
        analysisEnabled: tracks.isNotEmpty,
        analysisOverlayEnabled:
            platformCapabilities.analysisOverlays && tracks.isNotEmpty,
        nativePlaybackAvailable:
            platformCapabilities.nativePlayback ||
            platformCapabilities.localFilePlayback,
        localFilePlaybackAvailable: platformCapabilities.localFilePlayback,
        networkMediaAvailable: platformCapabilities.networkMediaPlayback,
        sshRemoteMediaAvailable: platformCapabilities.sshRemoteMediaPlayback,
        nativeFilePickerAvailable: platformCapabilities.nativeFilePicker,
        localFilePlaybackCapability:
            platformCapabilities.localFilePlaybackCapability,
        networkMediaPlaybackCapability:
            platformCapabilities.networkMediaPlaybackCapability,
        sshRemoteMediaPlaybackCapability:
            platformCapabilities.sshRemoteMediaPlaybackCapability,
        nativeFilePickerCapability:
            platformCapabilities.nativeFilePickerCapability,
        analysisOverlaysCapability:
            platformCapabilities.analysisOverlaysCapability,
        tracks: tracks,
        syncOffsets: syncOffsets,
        audibleTrackFileId: audibleTrackFileId,
        performanceAlertPolicy: performanceAlertPolicy,
        analysisDataSource: analysisDataSource,
      ),
      playback: MainWindowPlaybackVm(
        timelineStartWidth: timelineStartWidth,
        isPlaying: isPlaying,
        currentPtsUs: currentPtsUs,
        durationUs: durationUs,
        markerUs: markerUs,
        seekMinUs: seekMinUs,
        seekMaxUs: seekMaxUs,
        loopRangeEnabled: loopRangeEnabled,
        loopStartUs: loopStartUs,
        loopEndUs: loopEndUs,
        controlsWidth: controlsWidth,
      ),
      deck: MainWindowDeckVm(
        tab: deckTab,
        height: deckHeight,
        collapsed: deckCollapsed,
        analysisEntries: analysisEntries,
        analysisTestHosts: analysisTestHosts,
        qualityDataSource: qualityDataSource,
        analysisPlaybackByFileId: {
          for (final entry in presentedFrameAnchors.entries)
            entry.key: AnalysisPlaybackPosition(
              ptsUs: entry.value.ptsUs,
              dtsUs: entry.value.dtsUs,
              analysisFrameIndex: entry.value.analysisFrameIndex,
            ),
        },
      ),
      selection: selection,
      overlays: MainWindowOverlayVm(
        dragging: dragging,
        mediaInfoVisible: mediaInfoVisible,
        profilerVisible: profilerVisible,
        settingsVisible: settingsVisible,
        analysisOverlayControlsVisible: analysisOverlayControlsVisible,
        marksSidebarVisible: marksSidebarVisible,
        marksSidebarWidth: marksSidebarWidth,
        fullScreen: fullScreen,
        fullScreenControlsVisible: fullScreenControlsVisible,
      ),
    );
  }
}

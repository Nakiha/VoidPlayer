import 'dart:async';

import 'package:flutter/widgets.dart';

import '../../app_log.dart';
import '../../preferences/playback_preferences.dart';
import '../../remote/ssh_remote_media.dart';
import '../../track_manager.dart';
import '../../utils/async_guard.dart';
import '../../utils/media_source.dart';
import '../../video_renderer_controller.dart';
import '../../viewport/viewport_display_state.dart';
import '../native_file_picker.dart';
import 'main_window_layout.dart';
import 'main_window_media_lifecycle.dart';
import 'main_window_state.dart';
import 'main_window_timeline_metrics.dart';

class MainWindowMediaCoordinator {
  final NativePlayerController controller;
  final TrackManager trackManager;
  final MainWindowLayoutCoordinator layoutCoordinator;
  final MainWindowStateStore stateStore;
  final MainWindowTimelineMetrics timelineMetrics;
  final MainWindowMediaLifecycle lifecycle;
  final PlaybackPreferences playbackPreferences;
  final bool Function() mounted;
  final SshRemoteMediaService sshRemoteMedia;
  Future<void>? _loadInFlight;
  bool _disposed = false;

  MainWindowMediaCoordinator({
    required this.controller,
    required this.trackManager,
    required this.layoutCoordinator,
    required this.stateStore,
    required this.timelineMetrics,
    required this.lifecycle,
    required this.playbackPreferences,
    required this.mounted,
    this.sshRemoteMedia = const SshRemoteMediaService(),
  });

  void dispose() {
    _disposed = true;
  }

  bool get _alive => !_disposed && mounted();

  MainWindowStateModel get _state => stateStore.value;

  int? textureId() => _state.textureId;
  void setViewportState(ViewportDisplayState state) =>
      stateStore.setViewportState(state);
  void setTextureId(int textureId) => stateStore.setTextureId(textureId);
  void setLayout(LayoutState layout) => stateStore.setLayout(layout);
  Map<int, int> syncOffsets() => _state.syncOffsets;
  void setSyncOffsets(Map<int, int> offsets) =>
      stateStore.setSyncOffsets(offsets);
  int durationUs() => _state.durationUs;
  int? pendingSeekUs() => _state.pendingSeekUs;
  int currentPtsUs() => _state.currentPtsUs;
  int? audibleTrackFileId() => _state.audibleTrackFileId;
  void setAudibleTrackFileId(int? fileId) =>
      stateStore.setAudibleTrackFileId(fileId);

  Future<void> loadMediaPaths(List<String> paths) {
    if (paths.isEmpty) return Future<void>.value();
    if (_disposed) return Future<void>.value();

    final previous = _loadInFlight;
    late final Future<void> next;
    next =
        (previous == null ? Future<void>.value() : previous.catchError((_) {}))
            .then((_) => _loadMediaPathsImpl(paths))
            .whenComplete(() {
              if (identical(_loadInFlight, next)) {
                _loadInFlight = null;
              }
            });
    _loadInFlight = next;
    return next;
  }

  Future<void> _loadMediaPathsImpl(List<String> paths) async {
    if (!_alive) return;

    if (textureId() == null) {
      setViewportState(const ViewportDisplayState.loading());
      try {
        final initialWidth = layoutCoordinator.viewportWidth > 0
            ? layoutCoordinator.viewportWidth
            : 1920;
        final initialHeight = layoutCoordinator.viewportHeight > 0
            ? layoutCoordinator.viewportHeight
            : 1080;
        final res = await controller.createPlayer(
          paths,
          width: initialWidth,
          height: initialHeight,
          useHardwareDecode: playbackPreferences.useHardwareDecode,
        );
        if (!_alive) return;
        setTextureId(res.textureId);
        trackManager.setTracks(res.tracks);
        await _applyInitialPtsOffsets(res.tracks);
        if (!_alive) return;
        final nativeLayout = await controller.getLayout();
        if (!_alive) return;
        setLayout(
          nativeLayout.copyWith(
            pixelSizeMode:
                playbackPreferences.viewportPixelSizeMode.layoutValue,
            order: trackManager.order,
          ),
        );
        layoutCoordinator.markLayoutDirty();
        lifecycle.applyStartupLoopRangeIfReady();
        await WidgetsBinding.instance.endOfFrame;
        if (!_alive) return;
        if (layoutCoordinator.viewportWidth > 0 &&
            layoutCoordinator.viewportHeight > 0) {
          await controller.resize(
            layoutCoordinator.viewportWidth,
            layoutCoordinator.viewportHeight,
          );
        }
        if (!_alive) return;
        setViewportState(const ViewportDisplayState.active());
      } catch (e) {
        log.severe("createPlayer failed: $e");
        if (_alive) {
          setViewportState(ViewportDisplayState.error('Failed to load: $e'));
        }
      }
    } else {
      for (final path in paths) {
        if (!_alive) return;
        try {
          final previousTrackCount = trackManager.count;
          final track = await controller.addTrack(
            path,
            useHardwareDecode: playbackPreferences.useHardwareDecode,
          );
          if (!_alive) return;
          await layoutCoordinator.preemptTimelineTrackCountChange(
            previousCount: previousTrackCount,
            nextCount: previousTrackCount + 1,
          );
          if (!_alive) return;
          trackManager.addTrack(track);
          await _applyInitialPtsOffsets([track]);
          if (!_alive) return;
          lifecycle.applyStartupLoopRangeIfReady();
        } catch (e) {
          log.severe("addTrack failed: $e");
        }
      }
    }
  }

  void addMediaByPath(String path) {
    if (path.isEmpty) return;
    fireAndLog('add media by path', loadMediaPaths([path]));
  }

  Future<void> addNetworkMedia(String url) {
    final normalized = normalizeNetworkMediaUrl(url);
    if (normalized == null) return Future<void>.value();
    return loadMediaPaths([normalized]);
  }

  Future<void> addSshRemoteMedia(String remotePath) async {
    if (!_alive) return;
    try {
      final playableInput = sshRemoteMedia.playableInput(remotePath);
      await loadMediaPaths([playableInput]);
    } catch (e) {
      log.severe("SSH remote media failed: $e");
      if (_alive && textureId() == null) {
        setViewportState(ViewportDisplayState.error('Failed to load: $e'));
      }
      rethrow;
    }
  }

  Future<void> openFile() async {
    final paths = await WindowsNativeFilePicker.pickFiles(allowMultiple: true);
    if (paths == null || paths.isEmpty) return;
    await loadMediaPaths(paths);
  }

  Future<void> removeTrack(int fileId) async {
    if (!_alive) return;
    try {
      trackManager.entries.firstWhere(
        (e) => e.fileId == fileId,
        orElse: () => throw StateError('No track with fileId $fileId'),
      );
      final wasAudible = audibleTrackFileId() == fileId;

      await controller.removeTrack(fileId);
      if (!_alive) return;
      final tracks = await controller.getTracks();
      if (!_alive) return;
      if (tracks.isEmpty) {
        await controller.destroyPlayerOnly();
        if (!_alive) return;
        lifecycle.preparePlayerDestroyAfterLastTrackRemoved();
        lifecycle.resetAfterLastTrackRemoved();
      } else {
        final previousTrackCount = trackManager.count;
        await layoutCoordinator.preemptTimelineTrackCountChange(
          previousCount: previousTrackCount,
          nextCount: tracks.length,
        );
        if (!_alive) return;
        trackManager.setTracks(tracks);
        removeSyncOffset(fileId);
        if (wasAudible) {
          setAudibleTrackFileId(null);
          await controller.setAudibleTrack(null);
        }
      }
    } catch (e) {
      if (_alive) log.severe("removeTrack failed: $e");
    }
  }

  void onMediaSwapped(int slotIndex, int targetTrackIndex) {
    trackManager.swapTracks(slotIndex, targetTrackIndex);
  }

  Future<void> onOffsetChanged(int fileId, int deltaMs) async {
    if (!_alive) return;
    final currentOffsetUs = syncOffsets()[fileId] ?? 0;
    final newOffsetUs = currentOffsetUs + deltaMs * 1000;

    final entry = trackManager.entries.firstWhere(
      (e) => e.fileId == fileId,
      orElse: () => throw StateError('No track with fileId $fileId'),
    );

    await controller.setTrackOffset(
      fileId: entry.fileId,
      offsetUs: newOffsetUs,
    );
    if (!_alive) return;

    setSyncOffset(fileId, newOffsetUs);
    await refreshTracksAtCurrentPosition();
  }

  Future<void> onOffsetChangedForSlot(int slot, int deltaMs) async {
    if (!_alive) return;
    final entry = trackManager.entries.firstWhere(
      (e) => e.info.slot == slot,
      orElse: () => throw StateError('No track at slot $slot'),
    );
    await onOffsetChanged(entry.fileId, deltaMs);
  }

  Future<void> refreshTracksAtCurrentPosition() async {
    if (!_alive) return;
    var targetUs = pendingSeekUs() ?? currentPtsUs();
    if (pendingSeekUs() == null) {
      try {
        targetUs = await controller.currentPts();
      } catch (_) {
        targetUs = currentPtsUs();
      }
    }
    if (!_alive) return;

    final clampedTargetUs = targetUs
        .clamp(0, timelineMetrics.effectiveDurationUs)
        .toInt();
    lifecycle.seekTo(clampedTargetUs);
  }

  void removeSyncOffset(int fileId) {
    setSyncOffsets(Map.from(syncOffsets())..remove(fileId));
  }

  void setSyncOffset(int fileId, int offsetUs) {
    setSyncOffsets(Map.from(syncOffsets())..[fileId] = offsetUs);
  }

  Future<void> _applyInitialPtsOffsets(Iterable<TrackInfo> tracks) async {
    final nextOffsets = Map<int, int>.from(syncOffsets());
    var changed = false;

    for (final track in tracks) {
      final startTimeUs = track.startTimeUs;
      if (startTimeUs <= 0 || nextOffsets.containsKey(track.fileId)) {
        continue;
      }

      final offsetUs = -startTimeUs;
      try {
        await controller.setTrackOffset(
          fileId: track.fileId,
          offsetUs: offsetUs,
        );
      } catch (e) {
        log.warning(
          'auto initial PTS offset failed for fileId=${track.fileId}: $e',
        );
        continue;
      }
      if (!_alive) return;

      nextOffsets[track.fileId] = offsetUs;
      changed = true;
    }

    if (changed) {
      setSyncOffsets(nextOffsets);
    }
  }
}

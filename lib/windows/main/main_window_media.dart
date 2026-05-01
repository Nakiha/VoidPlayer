import 'dart:async';

import 'package:flutter/widgets.dart';

import '../../app_log.dart';
import '../../track_manager.dart';
import '../../utils/async_guard.dart';
import '../../video_renderer_controller.dart';
import 'main_window_layout.dart';
import '../native_file_picker.dart';

class MainWindowMediaCoordinator {
  final NativePlayerController controller;
  final TrackManager trackManager;
  final MainWindowLayoutCoordinator layoutCoordinator;
  final bool Function() mounted;
  final int? Function() textureId;
  final void Function(int state) setViewportState;
  final void Function(int textureId) setTextureId;
  final void Function(LayoutState layout) setLayout;
  final Map<int, int> Function() syncOffsets;
  final void Function(Map<int, int> offsets) setSyncOffsets;
  final int Function() durationUs;
  final int? Function() pendingSeekUs;
  final int Function() currentPtsUs;
  final int? Function() audibleTrackFileId;
  final void Function(int? fileId) setAudibleTrackFileId;
  final VoidCallback applyStartupLoopRangeIfReady;
  final VoidCallback cancelLoopBoundaryTimer;
  final VoidCallback resetAfterLastTrackRemoved;
  final void Function(int ptsUs) seekTo;
  Future<void>? _loadInFlight;
  bool _disposed = false;

  MainWindowMediaCoordinator({
    required this.controller,
    required this.trackManager,
    required this.layoutCoordinator,
    required this.mounted,
    required this.textureId,
    required this.setViewportState,
    required this.setTextureId,
    required this.setLayout,
    required this.syncOffsets,
    required this.setSyncOffsets,
    required this.durationUs,
    required this.pendingSeekUs,
    required this.currentPtsUs,
    required this.audibleTrackFileId,
    required this.setAudibleTrackFileId,
    required this.applyStartupLoopRangeIfReady,
    required this.cancelLoopBoundaryTimer,
    required this.resetAfterLastTrackRemoved,
    required this.seekTo,
  });

  void dispose() {
    _disposed = true;
  }

  bool get _alive => !_disposed && mounted();

  Future<void> loadMediaPaths(List<String> paths) {
    if (paths.isEmpty) return Future.value();
    if (_disposed) return Future.value();

    final previous = _loadInFlight;
    late final Future<void> next;
    next = (previous == null ? Future.value() : previous.catchError((_) {}))
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
      setViewportState(0);
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
        );
        if (!_alive) return;
        setTextureId(res.textureId);
        trackManager.setTracks(res.tracks);
        final nativeLayout = await controller.getLayout();
        if (!_alive) return;
        setLayout(nativeLayout);
        applyStartupLoopRangeIfReady();
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
        setViewportState(2);
      } catch (e) {
        log.severe("createPlayer failed: $e");
        if (_alive) setViewportState(1);
      }
    } else {
      for (final path in paths) {
        if (!_alive) return;
        try {
          final previousTrackCount = trackManager.count;
          final track = await controller.addTrack(path);
          if (!_alive) return;
          await layoutCoordinator.preemptTimelineTrackCountChange(
            previousCount: previousTrackCount,
            nextCount: previousTrackCount + 1,
          );
          if (!_alive) return;
          trackManager.addTrack(track);
          applyStartupLoopRangeIfReady();
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

  Future<void> openFile() async {
    final paths = await WindowsNativeFilePicker.pickFiles(allowMultiple: true);
    if (paths == null || paths.isEmpty) return;
    await loadMediaPaths(paths);
  }

  Future<void> removeTrack(int fileId) async {
    if (!_alive) return;
    try {
      final entry = trackManager.entries.firstWhere(
        (e) => e.fileId == fileId,
        orElse: () => throw StateError('No track with fileId $fileId'),
      );
      final slot = entry.info.slot;
      final wasAudible = audibleTrackFileId() == fileId;

      await controller.removeTrack(fileId);
      if (!_alive) return;
      final tracks = await controller.getTracks();
      if (!_alive) return;
      if (tracks.isEmpty) {
        await controller.destroyPlayerOnly();
        if (!_alive) return;
        cancelLoopBoundaryTimer();
        resetAfterLastTrackRemoved();
      } else {
        final previousTrackCount = trackManager.count;
        await layoutCoordinator.preemptTimelineTrackCountChange(
          previousCount: previousTrackCount,
          nextCount: tracks.length,
        );
        if (!_alive) return;
        trackManager.setTracks(tracks);
        removeSyncOffset(slot);
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
    trackManager.moveTrack(slotIndex, targetTrackIndex);
  }

  Future<void> onOffsetChanged(int slot, int deltaMs) async {
    final currentOffsetUs = syncOffsets()[slot] ?? 0;
    final newOffsetUs = currentOffsetUs + deltaMs * 1000;

    final entry = trackManager.entries.firstWhere(
      (e) => e.info.slot == slot,
      orElse: () => throw StateError('No track at slot $slot'),
    );

    await controller.setTrackOffset(
      fileId: entry.fileId,
      offsetUs: newOffsetUs,
    );
    if (!mounted()) return;

    setSyncOffset(slot, newOffsetUs);
    await refreshTracksAtCurrentPosition();
  }

  Future<void> refreshTracksAtCurrentPosition() async {
    var targetUs = pendingSeekUs() ?? currentPtsUs();
    if (pendingSeekUs() == null) {
      try {
        targetUs = await controller.currentPts();
      } catch (_) {
        targetUs = currentPtsUs();
      }
    }
    if (!mounted()) return;

    final clampedTargetUs = targetUs.clamp(0, effectiveDurationUs).toInt();
    seekTo(clampedTargetUs);
  }

  int get effectiveDurationUs {
    int maxEffective = durationUs();
    for (final entry in trackManager.entries) {
      final offsetUs = syncOffsets()[entry.info.slot] ?? 0;
      final effective = entry.info.durationUs + offsetUs;
      if (effective > maxEffective) maxEffective = effective;
    }
    return maxEffective;
  }

  void removeSyncOffset(int slot) {
    setSyncOffsets(Map.from(syncOffsets())..remove(slot));
  }

  void setSyncOffset(int slot, int offsetUs) {
    setSyncOffsets(Map.from(syncOffsets())..[slot] = offsetUs);
  }
}

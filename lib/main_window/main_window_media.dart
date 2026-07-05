import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;

import '../app_log.dart';
import '../config/app_settings_repository.dart';
import '../platform/native_file_picker.dart';
import '../preferences/playback_preferences.dart';
import '../remote/ssh_remote_media.dart';
import '../track_manager.dart';
import '../utils/async_guard.dart';
import '../utils/media_source.dart';
import '../video_renderer_controller.dart';
import '../viewport/viewport_display_state.dart';
import 'main_window_layout.dart';
import 'main_window_media_lifecycle.dart';
import 'main_window_state.dart';
import 'main_window_timeline_metrics.dart';

int? defaultAudibleTrackForPolicy({
  required DefaultAudioPlaybackPolicy policy,
  required int? currentAudibleTrack,
  required List<TrackInfo> addedTracks,
}) {
  switch (policy) {
    case DefaultAudioPlaybackPolicy.muted:
      return currentAudibleTrack;
    case DefaultAudioPlaybackPolicy.playFirstTrack:
      if (currentAudibleTrack != null) return currentAudibleTrack;
      return addedTracks.isEmpty ? null : addedTracks.first.fileId;
  }
}

class MainWindowMediaCoordinator {
  final NativePlayerController controller;
  final TrackManager trackManager;
  final MainWindowLayoutCoordinator layoutCoordinator;
  final MainWindowStateStore stateStore;
  final MainWindowTimelineMetrics timelineMetrics;
  final MainWindowMediaLifecycle lifecycle;
  final PlaybackPreferences playbackPreferences;
  final NativeFilePicker nativeFilePicker;
  final AppSettingsRepository appSettings;
  final bool Function() mounted;
  final ValueChanged<int>? onDuplicateMediaSkipped;
  final ValueChanged<String>? onMediaLoadRejected;
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
    this.nativeFilePicker = const MethodChannelNativeFilePicker(),
    required this.appSettings,
    required this.mounted,
    this.onDuplicateMediaSkipped,
    this.onMediaLoadRejected,
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

  Future<void> _syncDefaultAudioPolicy(List<TrackInfo> addedTracks) async {
    final nextAudibleTrack = defaultAudibleTrackForPolicy(
      policy: playbackPreferences.defaultAudioPlaybackPolicy,
      currentAudibleTrack: audibleTrackFileId(),
      addedTracks: addedTracks,
    );
    setAudibleTrackFileId(nextAudibleTrack);
    await controller.setAudibleTrack(nextAudibleTrack);
  }

  Future<void> loadMediaPaths(List<String> paths) {
    if (paths.isEmpty) return Future<void>.value();
    if (_disposed) return Future<void>.value();

    final previous = _loadInFlight;
    log.info(
      '[MediaLoad] enqueue count=${paths.length} queued=${previous != null} '
      'hasPlayer=${controller.hasPlayer} texture=${textureId()} '
      'tracks=${trackManager.count}',
    );
    late final Future<void> next;
    next =
        (previous == null
                ? Future<void>.value()
                : previous.catchError((Object error, StackTrace stack) {
                    log.warning(
                      'previous media load failed before queued load',
                      error,
                      stack,
                    );
                  }))
            .then((_) => _loadMediaPathsImpl(paths))
            .whenComplete(() {
              if (identical(_loadInFlight, next)) {
                _loadInFlight = null;
              }
              log.info(
                '[MediaLoad] complete count=${paths.length} '
                'hasPlayer=${controller.hasPlayer} texture=${textureId()} '
                'tracks=${trackManager.count}',
              );
            });
    _loadInFlight = next;
    return next;
  }

  Future<void> _loadMediaPathsImpl(List<String> paths) async {
    if (!_alive) return;
    await _activateSecurityScopedBookmarks(paths);
    if (!_alive) return;
    final uniquePaths = await _filterDuplicateMedia(paths);
    if (!_alive || uniquePaths.isEmpty) return;

    log.info(
      '[MediaLoad] begin unique=${uniquePaths.length} '
      'hasPlayer=${controller.hasPlayer} texture=${textureId()} '
      'tracks=${trackManager.count}',
    );
    if (!controller.hasPlayer || trackManager.isEmpty) {
      if (uniquePaths.length > TrackManager.maxTracks) {
        _rejectTrackLimit(requestedCount: uniquePaths.length);
        return;
      }
      setViewportState(const ViewportDisplayState.loading());
      try {
        final initialWidth = layoutCoordinator.viewportWidth > 0
            ? layoutCoordinator.viewportWidth
            : 1920;
        final initialHeight = layoutCoordinator.viewportHeight > 0
            ? layoutCoordinator.viewportHeight
            : 1080;
        log.info(
          '[MediaLoad] createPlayer start count=${uniquePaths.length} '
          'size=${initialWidth}x$initialHeight',
        );
        final res = await controller.createPlayer(
          uniquePaths,
          width: initialWidth,
          height: initialHeight,
          useHardwareDecode: playbackPreferences.useHardwareDecode,
        );
        log.info(
          '[MediaLoad] createPlayer native done texture=${res.textureId} '
          'tracks=${res.tracks.length}',
        );
        if (!_alive) return;
        setTextureId(res.textureId);
        trackManager.setTracks(res.tracks);
        log.info('[MediaLoad] createPlayer tracks committed');
        await _syncDefaultAudioPolicy(res.tracks);
        log.info('[MediaLoad] createPlayer audio policy synced');
        if (!_alive) return;
        await _applyInitialPtsOffsets(res.tracks);
        log.info('[MediaLoad] createPlayer pts offsets applied');
        if (!_alive) return;
        final nativeLayout = await controller.getLayout();
        log.info('[MediaLoad] createPlayer layout fetched');
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
        if (!_alive) return;
        if (layoutCoordinator.viewportWidth > 0 &&
            layoutCoordinator.viewportHeight > 0) {
          log.info(
            '[MediaLoad] createPlayer resize start '
            '${layoutCoordinator.viewportWidth}x${layoutCoordinator.viewportHeight}',
          );
          await controller.resize(
            layoutCoordinator.viewportWidth,
            layoutCoordinator.viewportHeight,
          );
          log.info('[MediaLoad] createPlayer resize done');
        }
        if (!_alive) return;
        setViewportState(const ViewportDisplayState.active());
        log.info('[MediaLoad] createPlayer active');
      } catch (e) {
        log.severe("createPlayer failed: $e");
        if (_alive) {
          setViewportState(ViewportDisplayState.error('Failed to load: $e'));
        }
      }
    } else {
      final availableSlots = TrackManager.maxTracks - trackManager.count;
      if (uniquePaths.length > availableSlots) {
        _rejectTrackLimit(
          requestedCount: trackManager.count + uniquePaths.length,
        );
        return;
      }
      for (final path in uniquePaths) {
        if (!_alive) return;
        try {
          final previousTrackCount = trackManager.count;
          log.info(
            '[MediaLoad] addTrack start previousTracks=$previousTrackCount '
            'path=$path',
          );
          final track = await controller.addTrack(
            path,
            useHardwareDecode: playbackPreferences.useHardwareDecode,
          );
          log.info(
            '[MediaLoad] addTrack native done fileId=${track.fileId} '
            'slot=${track.slot}',
          );
          if (!_alive) return;
          await layoutCoordinator.preemptTimelineTrackCountChange(
            previousCount: previousTrackCount,
            nextCount: previousTrackCount + 1,
          );
          log.info('[MediaLoad] addTrack preempt resize done');
          if (!_alive) return;
          trackManager.addTrack(track);
          log.info('[MediaLoad] addTrack track model committed');
          await _syncDefaultAudioPolicy([track]);
          log.info('[MediaLoad] addTrack audio policy synced');
          if (!_alive) return;
          await _applyInitialPtsOffsets([track]);
          log.info('[MediaLoad] addTrack pts offsets applied');
          if (!_alive) return;
          lifecycle.applyStartupLoopRangeIfReady();
          log.info('[MediaLoad] addTrack done fileId=${track.fileId}');
        } catch (e) {
          log.severe("addTrack failed: $e");
        }
      }
    }
  }

  void _rejectTrackLimit({required int requestedCount}) {
    final message =
        'VoidPlayer supports at most ${TrackManager.maxTracks} tracks. '
        'Requested $requestedCount.';
    log.warning(message);
    onMediaLoadRejected?.call(message);
    if (!controller.hasPlayer) {
      setViewportState(ViewportDisplayState.error(message));
    }
  }

  Future<List<String>> _filterDuplicateMedia(List<String> paths) async {
    final result = await filterDuplicateMediaSources(
      paths,
      existingSources: trackManager.entries.map((entry) => entry.path),
    );
    if (result.skippedCount > 0) {
      onDuplicateMediaSkipped?.call(result.skippedCount);
    }
    return result.uniqueSources;
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
      if (_alive && !controller.hasPlayer) {
        setViewportState(ViewportDisplayState.error('Failed to load: $e'));
      }
      rethrow;
    }
  }

  Future<void> openFile() async {
    final entries = await nativeFilePicker.pickFileEntries(allowMultiple: true);
    if (entries == null || entries.isEmpty) return;
    await _rememberSecurityScopedBookmarks(entries);
    final paths = entries.map((entry) => entry.path).toList(growable: false);
    await loadMediaPaths(paths);
  }

  Future<void> _rememberSecurityScopedBookmarks(
    List<NativePickedFile> entries,
  ) async {
    final selectedBookmarks = <String, String>{};
    for (final entry in entries) {
      final bookmark = entry.securityScopedBookmarkBase64;
      if (bookmark == null || bookmark.isEmpty) continue;
      selectedBookmarks[entry.path] = bookmark;
    }
    if (selectedBookmarks.isEmpty) return;

    appSettings.securityScopedBookmarks = {
      ...appSettings.securityScopedBookmarks,
      ...selectedBookmarks,
    };
    await appSettings.save();
  }

  Future<void> _activateSecurityScopedBookmarks(List<String> paths) async {
    final knownBookmarks = appSettings.securityScopedBookmarks;
    if (knownBookmarks.isEmpty) return;
    final bookmarksToActivate = <String, String>{};
    for (final path in paths) {
      final bookmark = knownBookmarks[path];
      if (bookmark == null || bookmark.isEmpty) continue;
      bookmarksToActivate[path] = bookmark;
    }
    if (bookmarksToActivate.isEmpty) return;

    try {
      final activations = await nativeFilePicker
          .activateSecurityScopedBookmarks(bookmarksToActivate);
      if (activations.isEmpty) return;
      final refreshedBookmarks = <String, String>{};
      for (final activation in activations) {
        final bookmark = activation.securityScopedBookmarkBase64;
        if (bookmark == null || bookmark.isEmpty) continue;
        final key = activation.path.isNotEmpty
            ? activation.path
            : activation.requestedPath;
        if (key.isEmpty) continue;
        refreshedBookmarks[key] = bookmark;
        if (activation.requestedPath.isNotEmpty &&
            activation.requestedPath != key) {
          refreshedBookmarks[activation.requestedPath] = bookmark;
        }
      }
      if (refreshedBookmarks.isNotEmpty) {
        appSettings.securityScopedBookmarks = {
          ...knownBookmarks,
          ...refreshedBookmarks,
        };
        await appSettings.save();
      }
      final failures = activations
          .where((activation) => !activation.activated)
          .map(
            (activation) => activation.error == null
                ? activation.requestedPath
                : '${activation.requestedPath}: ${activation.error}',
          )
          .where((message) => message.isNotEmpty)
          .toList(growable: false);
      if (failures.isNotEmpty) {
        log.warning(
          '[macOS sandbox] Failed to activate ${failures.length} security-scoped bookmark(s): ${failures.join("; ")}',
        );
      }
    } catch (e) {
      log.warning(
        '[macOS sandbox] Security-scoped bookmark activation failed: $e',
      );
    }
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
      } catch (error, stack) {
        log.fine(
          'currentPts fallback failed during track refresh',
          error,
          stack,
        );
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

class DuplicateMediaFilterResult {
  final List<String> uniqueSources;
  final int skippedCount;

  const DuplicateMediaFilterResult({
    required this.uniqueSources,
    required this.skippedCount,
  });
}

Future<DuplicateMediaFilterResult> filterDuplicateMediaSources(
  List<String> sources, {
  Iterable<String> existingSources = const [],
}) async {
  final known = <String>{};
  for (final source in existingSources) {
    known.add(await mediaSourceIdentity(source));
  }

  var skipped = 0;
  final unique = <String>[];
  for (final source in sources) {
    final identity = await mediaSourceIdentity(source);
    if (!known.add(identity)) {
      skipped += 1;
      continue;
    }
    unique.add(source);
  }

  return DuplicateMediaFilterResult(
    uniqueSources: unique,
    skippedCount: skipped,
  );
}

Future<String> mediaSourceIdentity(String source) async {
  final uri = Uri.tryParse(source);
  if (uri != null && uri.hasScheme && !_looksLikeWindowsDrivePath(source)) {
    if (uri.scheme == 'file') {
      return _localFileIdentity(uri.toFilePath(windows: Platform.isWindows));
    }
    return uri.normalizePath().toString();
  }

  return _localFileIdentity(source);
}

Future<String> _localFileIdentity(String source) async {
  try {
    return p.normalize(await File(source).resolveSymbolicLinks());
  } catch (error, stack) {
    logFine('media source identity fallback: source=$source', error, stack);
    return p.normalize(File(source).absolute.path);
  }
}

bool _looksLikeWindowsDrivePath(String source) {
  return source.length >= 2 &&
      source.codeUnitAt(1) == 0x3A &&
      ((source.codeUnitAt(0) >= 0x41 && source.codeUnitAt(0) <= 0x5A) ||
          (source.codeUnitAt(0) >= 0x61 && source.codeUnitAt(0) <= 0x7A));
}

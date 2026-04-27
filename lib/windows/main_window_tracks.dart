part of 'main_window.dart';

extension _MainWindowTracks on _MainWindowState {
  void _onRemoveTrack(int fileId) async {
    final entry = _trackManager.entries.firstWhere(
      (e) => e.fileId == fileId,
      orElse: () => throw StateError('No track with fileId $fileId'),
    );
    final slot = entry.info.slot;

    await _controller.removeTrack(fileId);
    final tracks = await _controller.getTracks();
    if (tracks.isEmpty) {
      await _controller.dispose();
      _cancelLoopBoundaryTimer();
      _resetAfterLastTrackRemoved();
    } else {
      _trackManager.setTracks(tracks);
      _removeSyncOffset(slot);
    }
  }

  void _onMediaSwapped(int slotIndex, int targetTrackIndex) {
    _trackManager.moveTrack(slotIndex, targetTrackIndex);
  }

  void _onOffsetChanged(int slot, int deltaMs) async {
    final currentOffsetUs = _syncOffsets[slot] ?? 0;
    final newOffsetUs = currentOffsetUs + deltaMs * 1000;

    final entry = _trackManager.entries.firstWhere(
      (e) => e.info.slot == slot,
      orElse: () => throw StateError('No track at slot $slot'),
    );

    await _controller.setTrackOffset(
      fileId: entry.fileId,
      offsetUs: newOffsetUs,
    );
    if (!mounted) return;

    _setSyncOffset(slot, newOffsetUs);
    await _refreshTracksAtCurrentPosition();
  }

  Future<void> _refreshTracksAtCurrentPosition() async {
    var targetUs = _pendingSeekUs ?? _currentPtsUs;
    if (_pendingSeekUs == null) {
      try {
        targetUs = await _controller.currentPts();
      } catch (_) {
        targetUs = _currentPtsUs;
      }
    }
    if (!mounted) return;

    final clampedTargetUs = targetUs.clamp(0, _effectiveDurationUs).toInt();
    _seekTo(clampedTargetUs);
  }

  /// Effective max duration accounting for per-track offsets.
  int get _effectiveDurationUs {
    int maxEffective = _durationUs;
    for (final entry in _trackManager.entries) {
      final offsetUs = _syncOffsets[entry.info.slot] ?? 0;
      final effective = entry.info.durationUs + offsetUs;
      if (effective > maxEffective) maxEffective = effective;
    }
    return maxEffective;
  }
}

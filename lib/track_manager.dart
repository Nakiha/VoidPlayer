import 'package:flutter/foundation.dart';
import 'video_renderer_controller.dart';

/// A track entry in the display-order list.
/// Wraps [TrackInfo] with convenience getters.
class TrackEntry {
  final TrackInfo info;
  const TrackEntry(this.info);

  int get fileId => info.fileId;
  int get slot => info.slot;
  String get path => info.path;
  String get fileName => path.split(RegExp(r'[/\\]')).last;
}

/// Single source of truth for track display order on the Flutter side.
///
/// Analogous to the PySide6 `TrackManager`. Owns the ordered list of tracks
/// and computes the `order` array (fileIds) sent to the native shader via
/// [applyLayout].
class TrackManager with ChangeNotifier {
  static const int maxTracks = 4;

  List<TrackEntry> _entries = [];

  /// Unmodifiable view of the current display-order list.
  List<TrackEntry> get entries => List.unmodifiable(_entries);

  int get count => _entries.length;
  bool get canAdd => count < maxTracks;
  bool get isEmpty => _entries.isEmpty;

  /// The order array to send to the native shader.
  /// `order[displayPosition] = fileId`, length 4, unused slots filled with -1.
  List<int> get order {
    final result = List.filled(4, -1);
    for (int i = 0; i < _entries.length && i < 4; i++) {
      result[i] = _entries[i].fileId;
    }
    return result;
  }

  /// Replace all tracks at once (after [createPlayer] or [getTracks]).
  void setTracks(List<TrackInfo> tracks) {
    final tracksByFileId = _validatedTracksByFileId(tracks);
    _entries = tracksByFileId.values.map(TrackEntry.new).toList();
    notifyListeners();
  }

  /// Refresh native metadata while preserving the current display order.
  ///
  /// Tracks no longer reported by native are removed. Newly reported tracks
  /// are appended in native order.
  void reconcileTracks(List<TrackInfo> tracks) {
    final remaining = _validatedTracksByFileId(tracks);
    final next = <TrackEntry>[];
    for (final entry in _entries) {
      final refreshed = remaining.remove(entry.fileId);
      if (refreshed != null) {
        next.add(TrackEntry(refreshed));
      }
    }
    for (final track in tracks) {
      final added = remaining.remove(track.fileId);
      if (added != null) {
        next.add(TrackEntry(added));
      }
    }
    _entries = next;
    notifyListeners();
  }

  /// Add a single track to the end of the display order.
  void addTrack(TrackInfo info) {
    final existingIndex = _entries.indexWhere(
      (entry) => entry.fileId == info.fileId,
    );
    if (existingIndex >= 0) {
      throw StateError(
        'Native returned duplicate track fileId ${info.fileId}.',
      );
    }
    if (_entries.length >= maxTracks) return;
    _entries.add(TrackEntry(info));
    notifyListeners();
  }

  Map<int, TrackInfo> _validatedTracksByFileId(List<TrackInfo> tracks) {
    if (tracks.length > maxTracks) {
      throw StateError(
        'Native returned ${tracks.length} tracks; maximum is $maxTracks.',
      );
    }
    final tracksByFileId = <int, TrackInfo>{};
    for (final track in tracks) {
      if (tracksByFileId.containsKey(track.fileId)) {
        throw StateError(
          'Native returned duplicate track fileId ${track.fileId}.',
        );
      }
      tracksByFileId[track.fileId] = track;
    }
    return tracksByFileId;
  }

  /// Remove a track by its [fileId].
  void removeTrack(int fileId) {
    _entries.removeWhere((e) => e.fileId == fileId);
    notifyListeners();
  }

  /// Move a track from [oldIndex] to [newIndex] in the display order (drag reorder).
  ///
  /// [newIndex] follows Flutter's ReorderableListView convention: when moving
  /// an item down, the reported destination still includes the removed item.
  void moveTrack(int oldIndex, int newIndex) {
    if (oldIndex < 0 || oldIndex >= _entries.length) return;
    var target = newIndex;
    if (oldIndex < target) {
      target -= 1;
    }
    if (oldIndex == target) return;

    final entry = _entries.removeAt(oldIndex);
    final clamped = target.clamp(0, _entries.length).toInt();
    _entries.insert(clamped, entry);
    notifyListeners();
  }

  /// Swap two display positions directly.
  ///
  /// This is used by the media header combo boxes, where the selected index is
  /// already an absolute display position rather than ReorderableListView's
  /// insertion index.
  void swapTracks(int firstIndex, int secondIndex) {
    if (firstIndex < 0 ||
        firstIndex >= _entries.length ||
        secondIndex < 0 ||
        secondIndex >= _entries.length ||
        firstIndex == secondIndex) {
      return;
    }
    final first = _entries[firstIndex];
    _entries[firstIndex] = _entries[secondIndex];
    _entries[secondIndex] = first;
    notifyListeners();
  }

  /// Clear all tracks.
  void clear() {
    _entries.clear();
    notifyListeners();
  }
}

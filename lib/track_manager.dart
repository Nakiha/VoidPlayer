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
    _entries = tracks.map((t) => TrackEntry(t)).toList();
    notifyListeners();
  }

  /// Add a single track to the end of the display order.
  void addTrack(TrackInfo info) {
    if (_entries.length >= maxTracks) return;
    _entries.add(TrackEntry(info));
    notifyListeners();
  }

  /// Remove a track by its [fileId].
  void removeTrack(int fileId) {
    _entries.removeWhere((e) => e.fileId == fileId);
    notifyListeners();
  }

  /// Move a track from [oldIndex] to [newIndex] in the display order (drag reorder).
  ///
  /// [newIndex] follows Flutter's ReorderableListView convention: it refers to
  /// the index in the list AFTER the item has been conceptually removed from
  /// [oldIndex]. This means [newIndex] can equal `_entries.length - 1` (append).
  void moveTrack(int oldIndex, int newIndex) {
    if (oldIndex == newIndex) return;
    if (oldIndex < 0 || oldIndex >= _entries.length) return;
    final entry = _entries.removeAt(oldIndex);
    // After removal the list is shorter; clamp newIndex to valid insert range.
    final clamped = newIndex.clamp(0, _entries.length);
    _entries.insert(clamped, entry);
    notifyListeners();
  }

  /// Clear all tracks.
  void clear() {
    _entries.clear();
    notifyListeners();
  }
}

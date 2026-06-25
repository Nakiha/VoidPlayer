import 'dart:math' as math;

import 'quick_mark.dart';

class QuickMarkFrameContext {
  final int currentPtsUs;
  final Map<int, QuickMarkAnchor> presentedFrameAnchors;

  const QuickMarkFrameContext({
    required this.currentPtsUs,
    this.presentedFrameAnchors = const {},
  });
}

class QuickMarkView {
  final List<QuickMark> allMarks;
  final List<QuickMark> visibleMarks;
  final Set<int> visibleMarkIds;
  final int? selectedMarkId;
  final int? visibleSelectedMarkId;

  const QuickMarkView({
    required this.allMarks,
    required this.visibleMarks,
    required this.visibleMarkIds,
    required this.selectedMarkId,
    required this.visibleSelectedMarkId,
  });
}

class QuickMarkStore {
  final List<QuickMark> marks;
  final int nextId;

  QuickMarkStore({List<QuickMark> marks = const [], int nextId = 1})
    : marks = List.unmodifiable(marks),
      nextId = _normalizedNextId(marks, nextId);

  static int _normalizedNextId(List<QuickMark> marks, int nextId) {
    var highestId = 0;
    for (final mark in marks) {
      highestId = math.max(highestId, mark.id);
    }
    return math.max(nextId, highestId + 1);
  }

  /// Merges marks loaded from persistence into the in-memory set.
  ///
  /// Loaded marks replace any current marks belonging to the same files.
  /// Persisted mark ids are only unique per media, so ids that collide with
  /// kept or previously merged marks are remapped to fresh ids; non-colliding
  /// ids stay stable to preserve thumbnail-cache hits.
  static QuickMarkStore mergeLoaded({
    required List<QuickMark> current,
    required List<QuickMark> loaded,
    int nextId = 1,
  }) {
    final loadedFileIds = {for (final mark in loaded) mark.fileId};
    final kept = [
      for (final mark in current)
        if (!loadedFileIds.contains(mark.fileId)) mark,
    ];
    final usedIds = {for (final mark in kept) mark.id};
    var counter = _normalizedNextId(kept, nextId);
    final merged = [...kept];
    for (final mark in loaded) {
      var id = mark.id;
      if (id <= 0 || usedIds.contains(id)) {
        while (usedIds.contains(counter)) {
          counter++;
        }
        id = counter++;
      }
      usedIds.add(id);
      merged.add(id == mark.id ? mark : mark.copyWith(id: id));
    }
    return QuickMarkStore(marks: merged, nextId: math.max(nextId, counter));
  }

  bool get isEmpty => marks.isEmpty;

  QuickMark? markById(int id) {
    for (final mark in marks) {
      if (mark.id == id) return mark;
    }
    return null;
  }

  bool contains(int id) => markById(id) != null;

  QuickMarkStore add(QuickMark mark) {
    final id = mark.id > 0 ? mark.id : nextId;
    return QuickMarkStore(
      marks: [
        ...marks,
        mark.copyWith(id: id),
      ],
      nextId: math.max(nextId, id + 1),
    );
  }

  QuickMarkStore update(QuickMark updated) {
    var found = false;
    final next = <QuickMark>[];
    for (final mark in marks) {
      if (mark.id == updated.id) {
        found = true;
        next.add(updated);
      } else {
        next.add(mark);
      }
    }
    if (!found) return this;
    return QuickMarkStore(marks: next, nextId: nextId);
  }

  QuickMarkStore delete(int id) {
    final next = marks.where((mark) => mark.id != id).toList(growable: false);
    if (next.length == marks.length) return this;
    return QuickMarkStore(marks: next, nextId: nextId);
  }

  QuickMarkStore deleteForFileId(int fileId) {
    final next = marks
        .where((mark) => mark.fileId != fileId)
        .toList(growable: false);
    if (next.length == marks.length) return this;
    return QuickMarkStore(marks: next, nextId: nextId);
  }

  bool isVisible(QuickMark mark, QuickMarkFrameContext context) {
    final currentAnchor = context.presentedFrameAnchors[mark.fileId];
    final toleranceUs = _visibilityToleranceUs(mark.anchor, currentAnchor);
    if (currentAnchor != null) {
      return mark.anchor.matchesPresentedFrameOrTime(
        currentAnchor,
        fallbackToleranceUs: toleranceUs,
      );
    }
    return (mark.ptsUs - context.currentPtsUs).abs() <= toleranceUs;
  }

  int _visibilityToleranceUs(
    QuickMarkAnchor markAnchor,
    QuickMarkAnchor? currentAnchor,
  ) {
    var toleranceUs = markAnchor.durationUs > 0
        ? (markAnchor.durationUs / 2).round()
        : 0;
    if (currentAnchor != null && currentAnchor.durationUs > 0) {
      toleranceUs = math.max(
        toleranceUs,
        (currentAnchor.durationUs / 2).round(),
      );
    }
    return toleranceUs;
  }

  QuickMarkView view({
    required QuickMarkFrameContext context,
    required int? selectedMarkId,
  }) {
    final visibleMarks = marks
        .where((mark) => isVisible(mark, context))
        .toList(growable: false);
    final visibleMarkIds = visibleMarks.map((mark) => mark.id).toSet();
    return QuickMarkView(
      allMarks: marks,
      visibleMarks: visibleMarks,
      visibleMarkIds: visibleMarkIds,
      selectedMarkId: selectedMarkId,
      visibleSelectedMarkId: visibleMarkIds.contains(selectedMarkId)
          ? selectedMarkId
          : null,
    );
  }
}

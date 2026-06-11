import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:flutter/material.dart';
import 'package:path/path.dart' as p;

import '../analysis/file_hash.dart';
import '../app_log.dart';
import '../app_paths.dart';
import '../marks/quick_mark.dart';
import '../marks/quick_mark_export.dart';
import '../marks/quick_mark_persistence.dart';
import '../marks/quick_mark_store.dart';
import '../marks/quick_mark_thumbnail.dart';
import '../storage/storage_catalog.dart';
import '../track_manager.dart';
import '../utils/async_guard.dart';
import '../video_renderer_controller.dart';
import '../viewport/display_geometry.dart';
import 'main_window_layout.dart';
import 'main_window_playback.dart';
import 'main_window_state.dart';

class MainWindowQuickMarkCoordinator {
  final NativePlayerController player;
  final TrackManager trackManager;
  final MainWindowStateStore stateStore;
  final MainWindowLayoutCoordinator layoutCoordinator;
  final MainWindowPlaybackCoordinator playbackCoordinator;
  final QuickMarkRepository repository;
  final bool Function() mounted;
  final bool Function() shuttingDown;

  ViewportSourceHit? _dragStart;
  Offset? _dragLatestPhysicalPosition;
  QuickMarkAnchor? _dragAnchor;
  Future<QuickMarkAnchor>? _dragAnchorFuture;
  int _dragSerial = 0;
  int _nextQuickMarkId = 1;
  int _persistenceSerial = 0;
  String _trackSignature = '';
  Timer? _saveTimer;
  Timer? _thumbnailTimer;
  Future<void>? _saveInFlight;
  bool _savePending = false;
  bool _thumbnailCaptureInFlight = false;
  bool _thumbnailRerunRequested = false;
  final Map<int, String> _mediaHashes = <int, String>{};
  List<QuickMarkMediaRef> _pendingSaveRefs = const [];
  List<QuickMark> _pendingSaveMarks = const [];

  MainWindowQuickMarkCoordinator({
    required this.player,
    required this.trackManager,
    required this.stateStore,
    required this.layoutCoordinator,
    required this.playbackCoordinator,
    required this.repository,
    required this.mounted,
    required this.shuttingDown,
  });

  QuickMarkView get view =>
      store.view(context: frameContext, selectedMarkId: _selectedQuickMarkId);

  QuickMarkStore get store =>
      QuickMarkStore(marks: _quickMarks, nextId: _nextQuickMarkId);

  QuickMarkFrameContext get frameContext => QuickMarkFrameContext(
    currentPtsUs: _currentPtsUs,
    presentedFrameAnchors: _presentedFrameAnchors,
  );

  QuickMark? get draft => _state.quickMarkDraft;
  Map<int, QuickMarkThumbnail> get thumbnails => _state.quickMarkThumbnails;

  void dispose() {
    _saveTimer?.cancel();
    _thumbnailTimer?.cancel();
  }

  Future<void> closeGracefully() async {
    dispose();
    await flushSave();
  }

  void handleStateChanged() {
    if (thumbnails.values.any(
      (thumbnail) => thumbnail.status == QuickMarkThumbnailStatus.queued,
    )) {
      _scheduleThumbnailCapture();
    }
  }

  void reconcilePersistence() {
    final refs = _mediaRefs();
    final signature = _mediaSignature(refs);
    if (signature == _trackSignature) return;
    _trackSignature = signature;
    final serial = ++_persistenceSerial;
    fireAndLogFine(
      'load quick marks',
      _loadForMediaRefs(refs, signature, serial),
    );
  }

  Future<void> flushSave() async {
    _saveTimer?.cancel();
    _saveTimer = null;
    await _savePendingNow();
  }

  void startDrag(Offset physicalPosition) {
    if (_textureId == null || trackManager.isEmpty) return;
    final hit = _projection()?.hitTestPhysical(physicalPosition);
    if (hit == null) return;
    if (_isPlaying) {
      fireAndLog('pause playback for quick mark', playbackCoordinator.pause());
    }
    stateStore.setSelectedQuickMarkId(null);
    _dragStart = hit;
    _dragLatestPhysicalPosition = physicalPosition;
    final serial = ++_dragSerial;
    final initialAnchor =
        _presentedFrameAnchors[hit.fileId] ??
        QuickMarkAnchor.fromPresentedFrame(
          fileId: hit.fileId,
          timing: null,
          fallbackPtsUs: _currentPtsUs,
        );
    _dragAnchor = initialAnchor;
    _dragAnchorFuture = _anchorForFileId(hit.fileId);
    stateStore.setQuickMarkDraft(
      _draftForDrag(start: hit, end: hit.sourceUv, anchor: initialAnchor),
    );
    final anchorFuture = _dragAnchorFuture;
    if (anchorFuture != null) {
      fireAndLogFine(
        'resolve quick mark drag anchor',
        anchorFuture.then((anchor) {
          if (_dragSerial != serial || _dragStart != hit) return;
          _dragAnchor = anchor;
          final latest = _dragLatestPhysicalPosition;
          if (latest != null) {
            _updateDraftForDrag(latest);
          }
        }),
      );
    }
  }

  void updateDrag(Offset physicalPosition) {
    _dragLatestPhysicalPosition = physicalPosition;
    _updateDraftForDrag(physicalPosition);
  }

  void finishDrag() async {
    final serial = _dragSerial;
    final anchorFuture = _dragAnchorFuture;
    if (anchorFuture != null) {
      final anchor = await anchorFuture;
      if (_dragSerial != serial) return;
      if (_dragSerial == serial && _dragStart != null) {
        _dragAnchor = anchor;
        final latest = _dragLatestPhysicalPosition;
        if (latest != null) {
          _updateDraftForDrag(latest);
        }
      }
    }
    final activeDraft = draft;
    _dragStart = null;
    _dragLatestPhysicalPosition = null;
    _dragAnchor = null;
    _dragAnchorFuture = null;
    _dragSerial++;
    stateStore.setQuickMarkDraft(null);
    if (activeDraft == null ||
        activeDraft.sourceRect.width < 0.002 ||
        activeDraft.sourceRect.height < 0.002) {
      return;
    }
    _applyStore(store.add(activeDraft));
  }

  void cancelDrag() {
    _dragStart = null;
    _dragLatestPhysicalPosition = null;
    _dragAnchor = null;
    _dragAnchorFuture = null;
    _dragSerial++;
    stateStore.setQuickMarkDraft(null);
  }

  void select(int? id) {
    if (id != null) {
      final mark = store.markById(id);
      if (mark == null || !isVisible(mark)) return;
    }
    stateStore.setSelectedQuickMarkId(id);
  }

  void update(QuickMark updated) {
    _applyStore(store.update(updated));
  }

  void delete(int id) {
    _applyStore(store.delete(id));
    if (_selectedQuickMarkId == id) {
      stateStore.setSelectedQuickMarkId(null);
    }
  }

  void deleteForFileId(int fileId) {
    final nextStore = store.deleteForFileId(fileId);
    _applyStore(nextStore);
    final selectedId = _selectedQuickMarkId;
    if (selectedId != null && !nextStore.contains(selectedId)) {
      stateStore.setSelectedQuickMarkId(null);
    }
  }

  void focus(int id) {
    final mark = store.markById(id);
    if (mark == null || !isVisible(mark)) return;
    stateStore.setSelectedQuickMarkId(id);
    layoutCoordinator.focusQuickMark(mark);
  }

  void jumpTo(int id) {
    final mark = store.markById(id);
    if (mark == null) return;
    if (!isVisible(mark)) {
      playbackCoordinator.seekTo(mark.anchor.ptsUs);
    }
    stateStore.setSelectedQuickMarkId(id);
  }

  bool isVisible(QuickMark mark) {
    return store.isVisible(mark, frameContext);
  }

  /// Injects an agent-authored mark anchored to the current presented frame
  /// of the track in [slotIndex]. This is the inbound half of the review
  /// loop: algorithms flag candidate regions, a human confirms or rejects.
  Future<void> addAgentMark({
    required int slotIndex,
    required Rect sourceRect,
    String? defectType,
    int? severity,
    String text = '',
  }) async {
    final entries = trackManager.entries;
    if (slotIndex < 0 || slotIndex >= entries.length) {
      throw RangeError(
        'slot $slotIndex out of range (${entries.length} tracks)',
      );
    }
    final anchor = await _anchorForFileId(entries[slotIndex].fileId);
    _applyStore(
      store.add(
        QuickMark(
          id: 0,
          anchor: anchor,
          sourceRect: sourceRect,
          origin: QuickMarkOrigin.agent,
          defectType: defectType,
          severity: severity,
          text: text,
        ),
      ),
    );
  }

  int get markCount => _quickMarks.length;

  /// Deletes every mark in the session and persists the empty set.
  void clearAllMarks() {
    if (_quickMarks.isEmpty) return;
    _applyStore(QuickMarkStore(marks: const [], nextId: _nextQuickMarkId));
    stateStore.setSelectedQuickMarkId(null);
  }

  /// Builds the versioned verdict export document for the current session:
  /// every loaded track with its lineage plus all in-memory marks including
  /// judgment fields. This is the outbound channel of the review loop.
  Future<Map<String, Object?>> buildMarksExportDocument() async {
    return buildQuickMarkExportDocument(
      media: await buildExportMedia(),
      marks: _quickMarks,
      generatedAtMs: DateTime.now().millisecondsSinceEpoch,
    );
  }

  /// Describes every loaded track with its media hash and source lineage.
  Future<List<QuickMarkExportMedia>> buildExportMedia() async {
    final entries = trackManager.entries;
    final media = <QuickMarkExportMedia>[];
    final catalog = StorageCatalog.defaultLocation();
    for (var slot = 0; slot < entries.length; slot++) {
      final entry = entries[slot];
      final hash = await _mediaHashForFileId(entry.fileId);
      media.add(
        QuickMarkExportMedia(
          fileId: entry.fileId,
          slotIndex: slot,
          path: entry.path,
          mediaHash: hash,
          sourceId: hash == null ? null : catalog.sourceIdForMediaHash(hash),
        ),
      );
    }
    return media;
  }

  Future<void> exportMarksToFile(String path) async {
    final document = await buildMarksExportDocument();
    final file = File(path);
    await file.parent.create(recursive: true);
    await file.writeAsString(jsonEncode(document), flush: true);
    log.info('[QuickMark] exported ${_quickMarks.length} marks to $path');
  }

  /// Declares the source lineage of the track in [slotIndex]: which original
  /// clip this media is an encode of. The id lands in the storage catalog so
  /// annotations can be joined across encodes of the same source.
  Future<void> declareSourceIdForSlot(int slotIndex, String sourceId) async {
    final entries = trackManager.entries;
    if (slotIndex < 0 || slotIndex >= entries.length) {
      throw RangeError(
        'slot $slotIndex out of range (${entries.length} tracks)',
      );
    }
    final fileId = entries[slotIndex].fileId;
    final mediaHash = await _mediaHashForFileId(fileId);
    if (mediaHash == null) {
      throw StateError('no media hash for slot $slotIndex (fileId $fileId)');
    }
    StorageCatalog.defaultLocation().setMediaSourceId(
      mediaHash: mediaHash,
      sourceId: sourceId,
    );
  }

  MainWindowStateModel get _state => stateStore.value;

  int? get _textureId => _state.textureId;
  bool get _isPlaying => _state.isPlaying;
  int get _currentPtsUs => _state.currentPtsUs;
  LayoutState get _layout => _state.layout;
  List<QuickMark> get _quickMarks => _state.quickMarks;
  int? get _selectedQuickMarkId => _state.selectedQuickMarkId;
  Map<int, QuickMarkAnchor> get _presentedFrameAnchors =>
      _state.presentedFrameAnchors;

  List<QuickMarkMediaRef> _mediaRefs() => [
    for (final entry in trackManager.entries)
      QuickMarkMediaRef(fileId: entry.fileId, path: entry.path),
  ];

  Future<List<QuickMarkMediaRef>> _mediaRefsWithHashes(
    List<QuickMarkMediaRef> refs,
  ) async {
    final activeFileIds = refs.map((ref) => ref.fileId).toSet();
    _mediaHashes.removeWhere((fileId, _) => !activeFileIds.contains(fileId));
    final resolved = <QuickMarkMediaRef>[];
    for (final ref in refs) {
      final hash =
          ref.mediaHash ??
          _mediaHashes[ref.fileId] ??
          await _mediaHashForPath(ref.path, ref.mediaId);
      _mediaHashes[ref.fileId] = hash;
      resolved.add(
        QuickMarkMediaRef(
          fileId: ref.fileId,
          path: ref.path,
          mediaId: ref.mediaId,
          mediaHash: hash,
        ),
      );
    }
    return resolved;
  }

  Future<String> _mediaHashForPath(String path, String mediaId) async {
    Object? failure;
    StackTrace? failureStack;
    try {
      final file = File(path);
      if (await file.exists()) return computeFileSha256(path);
    } catch (error, stack) {
      failure = error;
      failureStack = stack;
    }
    // Remote media never has local content; the media-id hash is its stable
    // identity. For local files this degradation makes marks land in a
    // fallback bucket, so make it visible.
    if (!mediaId.contains('://')) {
      logWarning(
        '[QuickMark] media hash fallback: path=$path mediaId=$mediaId',
        failure,
        failureStack,
      );
    }
    return QuickMarkMediaRef.fallbackHashForMediaId(mediaId);
  }

  Future<String?> _mediaHashForFileId(int fileId) async {
    final cached = _mediaHashes[fileId];
    if (cached != null && cached.isNotEmpty) return cached;
    for (final entry in trackManager.entries) {
      if (entry.fileId != fileId) continue;
      final hash = await _mediaHashForPath(
        entry.path,
        QuickMarkMediaRef.mediaIdForPath(entry.path),
      );
      _mediaHashes[fileId] = hash;
      return hash;
    }
    return null;
  }

  String _mediaSignature(List<QuickMarkMediaRef> refs) {
    return refs.map((ref) => '${ref.fileId}:${ref.mediaId}').join('|');
  }

  Future<void> _loadForMediaRefs(
    List<QuickMarkMediaRef> refs,
    String signature,
    int serial,
  ) async {
    await flushSave();
    if (serial != _persistenceSerial || signature != _trackSignature) return;
    if (refs.isEmpty) return;
    try {
      final resolvedRefs = await _mediaRefsWithHashes(refs);
      if (serial != _persistenceSerial || signature != _trackSignature) return;
      final loaded = await repository.loadForMediaRefs(resolvedRefs);
      if (serial != _persistenceSerial || signature != _trackSignature) return;
      _applyStore(
        QuickMarkStore.mergeLoaded(
          current: _quickMarks,
          loaded: loaded,
          nextId: _nextQuickMarkId,
        ),
        persist: false,
      );
    } catch (e, stack) {
      log.warning('[QuickMark] load failed', e, stack);
    }
  }

  void _scheduleSave() {
    _pendingSaveRefs = _mediaRefs();
    _pendingSaveMarks = List<QuickMark>.unmodifiable(_quickMarks);
    _savePending = true;
    _saveTimer?.cancel();
    _saveTimer = Timer(
      const Duration(milliseconds: 300),
      () => fireAndLogFine('save quick marks', _savePendingNow()),
    );
  }

  Future<void> _savePendingNow() async {
    final previous = _saveInFlight;
    if (previous != null) {
      try {
        await previous;
      } catch (error, stack) {
        logFine('[QuickMark] previous save failed before retry', error, stack);
      }
    }
    if (!_savePending) return;
    final refs = _pendingSaveRefs;
    final marks = _pendingSaveMarks;
    _savePending = false;
    _pendingSaveRefs = const [];
    _pendingSaveMarks = const [];
    if (refs.isEmpty) return;
    late final List<QuickMarkMediaRef> resolvedRefs;
    try {
      resolvedRefs = await _mediaRefsWithHashes(refs);
    } catch (e, stack) {
      log.warning('[QuickMark] media hash resolution failed', e, stack);
      return;
    }
    final save = repository.saveForMediaRefs(resolvedRefs, marks);
    _saveInFlight = save;
    try {
      await save;
    } catch (e, stack) {
      log.warning('[QuickMark] save failed', e, stack);
    } finally {
      if (identical(_saveInFlight, save)) {
        _saveInFlight = null;
      }
    }
  }

  void _scheduleThumbnailCapture({
    Duration delay = const Duration(milliseconds: 160),
  }) {
    if (!mounted() || shuttingDown()) return;
    if (_thumbnailCaptureInFlight) {
      _thumbnailRerunRequested = true;
      return;
    }
    _thumbnailTimer?.cancel();
    _thumbnailTimer = Timer(delay, () {
      _thumbnailTimer = null;
      fireAndLogFine(
        'capture quick mark thumbnails',
        _captureQueuedThumbnails(),
      );
    });
  }

  Future<void> _captureQueuedThumbnails() async {
    if (_thumbnailCaptureInFlight || !mounted() || !player.hasPlayer) {
      return;
    }
    final projection = _projection();
    if (projection == null) return;

    _thumbnailCaptureInFlight = true;
    try {
      final queued =
          thumbnails.values
              .where(
                (thumbnail) =>
                    thumbnail.status == QuickMarkThumbnailStatus.queued,
              )
              .toList(growable: false)
            ..sort((a, b) => a.markId.compareTo(b.markId));
      for (final thumbnail in queued) {
        if (!mounted() || !player.hasPlayer) return;
        final latest = thumbnails[thumbnail.markId];
        if (latest == null ||
            latest.sourceKey != thumbnail.sourceKey ||
            latest.status != QuickMarkThumbnailStatus.queued) {
          continue;
        }
        final mark = store.markById(thumbnail.markId);
        if (mark == null || !isVisible(mark)) continue;
        final rect = _thumbnailViewportRect(projection, mark);
        if (rect == null) continue;

        try {
          final mediaHash = await _mediaHashForFileId(mark.fileId);
          if (mediaHash == null) continue;
          final renderDigest = _thumbnailDigest(thumbnail);
          final outputPath = _thumbnailOutputPath(
            mediaHash: mediaHash,
            markId: mark.id,
            renderDigest: renderDigest,
          );
          await Directory(p.dirname(outputPath)).create(recursive: true);
          final capture = await player.captureViewportRegion(
            x: rect.left,
            y: rect.top,
            width: rect.width,
            height: rect.height,
            maxSize: 160,
            outputPath: outputPath,
          );
          final assetPath = capture.outputPath ?? outputPath;
          final assetFile = File(assetPath);
          if (await assetFile.exists()) {
            final bytes = await assetFile.length();
            StorageCatalog.defaultLocation().registerThumbnail(
              mediaHash: mediaHash,
              markId: mark.id,
              renderDigest: renderDigest,
              path: assetPath,
              bytes: bytes,
            );
          }
          _replaceThumbnail(
            thumbnail.markId,
            thumbnail.copyWith(
              status: QuickMarkThumbnailStatus.ready,
              assetPath: assetPath,
              error: null,
            ),
          );
        } catch (e, stack) {
          log.warning('[QuickMark] thumbnail capture failed', e, stack);
          _replaceThumbnail(
            thumbnail.markId,
            thumbnail.copyWith(
              status: QuickMarkThumbnailStatus.failed,
              assetPath: null,
              error: e.toString(),
            ),
          );
        }
      }
    } finally {
      _thumbnailCaptureInFlight = false;
      if (_thumbnailRerunRequested) {
        _thumbnailRerunRequested = false;
        _scheduleThumbnailCapture();
      }
    }
  }

  ({int left, int top, int width, int height})? _thumbnailViewportRect(
    ViewportLayoutProjection projection,
    QuickMark mark,
  ) {
    final projected = projection.viewportProjectionForSourceRect(
      mark.fileId,
      mark.sourceRect,
    );
    if (projected == null) return null;
    final bounds = Rect.fromLTWH(
      0,
      0,
      projection.viewportWidth.toDouble(),
      projection.viewportHeight.toDouble(),
    );
    final rect = projected.viewportRect
        .intersect(projected.clipRect)
        .intersect(bounds);
    if (rect.isEmpty) return null;
    final targetRect = mark.shape == QuickMarkShape.arrow
        ? _arrowThumbnailViewportRect(projected, mark, rect)
        : rect;
    if (targetRect.isEmpty) return null;
    return _roundedViewportRect(
      targetRect.intersect(projected.clipRect).intersect(bounds),
      projection,
    );
  }

  Rect _arrowThumbnailViewportRect(
    ViewportProjectedSourceRect projected,
    QuickMark mark,
    Rect visibleRect,
  ) {
    final center = _sourcePointToViewportRect(
      projected.viewportRect,
      mark.sourceRect,
      mark.effectiveSourceEnd,
    );
    return Rect.fromCenter(
      center: center,
      width: visibleRect.width,
      height: visibleRect.height,
    );
  }

  Offset _sourcePointToViewportRect(
    Rect viewportRect,
    Rect sourceRect,
    Offset sourcePoint,
  ) {
    final tx = sourceRect.width.abs() <= 1e-6
        ? 0.5
        : (sourcePoint.dx - sourceRect.left) / sourceRect.width;
    final ty = sourceRect.height.abs() <= 1e-6
        ? 0.5
        : (sourcePoint.dy - sourceRect.top) / sourceRect.height;
    return Offset(
      viewportRect.left + viewportRect.width * tx,
      viewportRect.top + viewportRect.height * ty,
    );
  }

  ({int left, int top, int width, int height})? _roundedViewportRect(
    Rect rect,
    ViewportLayoutProjection projection,
  ) {
    if (rect.isEmpty) return null;
    final left = rect.left.floor().clamp(0, projection.viewportWidth).toInt();
    final top = rect.top.floor().clamp(0, projection.viewportHeight).toInt();
    final right = rect.right
        .ceil()
        .clamp(left, projection.viewportWidth)
        .toInt();
    final bottom = rect.bottom
        .ceil()
        .clamp(top, projection.viewportHeight)
        .toInt();
    final width = right - left;
    final height = bottom - top;
    if (width <= 0 || height <= 0) return null;
    return (left: left, top: top, width: width, height: height);
  }

  String _thumbnailDigest(QuickMarkThumbnail thumbnail) {
    return _thumbnailDigestForSourceKey(thumbnail.sourceKey);
  }

  String _thumbnailDigestForSourceKey(String sourceKey) {
    return sha1.convert(utf8.encode(sourceKey)).toString().substring(0, 16);
  }

  String _thumbnailOutputPath({
    required String mediaHash,
    required int markId,
    required String renderDigest,
  }) {
    return p.join(
      StorageCatalog.thumbnailDirectory(
        rootDir: AppPaths.current.rootDir,
        mediaHash: mediaHash,
      ),
      'mark_${markId}_$renderDigest.png',
    );
  }

  void _replaceThumbnail(int markId, QuickMarkThumbnail thumbnail) {
    final current = thumbnails[markId];
    if (current == null || current.sourceKey != thumbnail.sourceKey) return;
    stateStore.setQuickMarkThumbnails(
      Map.unmodifiable({...thumbnails, markId: thumbnail}),
    );
  }

  Future<void> _hydrateThumbnailsFromCatalog(List<QuickMark> marks) async {
    if (marks.isEmpty || thumbnails.isEmpty) return;
    if (!await File(AppPaths.current.storageDatabaseFile).exists()) return;
    final updates = <int, QuickMarkThumbnail>{};
    final catalog = StorageCatalog.defaultLocation();
    for (final mark in marks) {
      final thumbnail = thumbnails[mark.id];
      if (thumbnail == null ||
          thumbnail.status != QuickMarkThumbnailStatus.queued) {
        continue;
      }
      final mediaHash = await _mediaHashForFileId(mark.fileId);
      if (mediaHash == null) continue;
      final cached = (() {
        try {
          return catalog.findThumbnail(
            mediaHash: mediaHash,
            markId: mark.id,
            renderDigest: _thumbnailDigest(thumbnail),
          );
        } catch (error, stack) {
          logFine(
            '[QuickMark] thumbnail catalog lookup failed: '
            'markId=${mark.id} fileId=${mark.fileId}',
            error,
            stack,
          );
          return null;
        }
      })();
      if (cached == null) continue;
      updates[mark.id] = thumbnail.copyWith(
        status: QuickMarkThumbnailStatus.ready,
        assetPath: cached.path,
        error: null,
      );
    }
    if (!mounted() || updates.isEmpty) return;
    final next = Map<int, QuickMarkThumbnail>.of(thumbnails);
    var changed = false;
    for (final entry in updates.entries) {
      final current = next[entry.key];
      final updated = entry.value;
      if (current == null || current.sourceKey != updated.sourceKey) continue;
      next[entry.key] = updated;
      changed = true;
    }
    if (changed) {
      stateStore.setQuickMarkThumbnails(Map.unmodifiable(next));
    }
  }

  ViewportLayoutProjection? _projection() {
    final viewportWidth = layoutCoordinator.viewportWidth;
    final viewportHeight = layoutCoordinator.viewportHeight;
    if (viewportWidth <= 0 || viewportHeight <= 0 || trackManager.isEmpty) {
      return null;
    }
    return computeViewportLayoutProjection(
      viewportWidth: viewportWidth,
      viewportHeight: viewportHeight,
      layout: _layout,
      tracks: trackManager.entries
          .map((entry) => DisplayTrackGeometry.fromTrackInfo(entry.info))
          .toList(),
    );
  }

  Future<QuickMarkAnchor> _anchorForFileId(int fileId) async {
    PresentedFrameTiming? timing;
    try {
      timing = await player.currentPresentedFrame(fileId);
    } catch (error, stack) {
      logFine(
        '[QuickMark] presented frame fallback: fileId=$fileId',
        error,
        stack,
      );
      timing = null;
    }
    return QuickMarkAnchor.fromPresentedFrame(
      fileId: fileId,
      timing: timing,
      fallbackPtsUs: _currentPtsUs,
    );
  }

  QuickMark _draftForDrag({
    required ViewportSourceHit start,
    required Offset end,
    required QuickMarkAnchor anchor,
  }) {
    return QuickMark(
      id: 0,
      anchor: anchor,
      sourceRect: Rect.fromPoints(start.sourceUv, end),
      sourceStart: start.sourceUv,
      sourceEnd: end,
    );
  }

  void _updateDraftForDrag(Offset physicalPosition) {
    final start = _dragStart;
    final anchor = _dragAnchor;
    if (start == null || anchor == null) return;
    final projection = _projection();
    final end = projection?.sourceUvForTrackPhysical(
      start.fileId,
      physicalPosition,
      clipToVisibleRegion: true,
    );
    if (end == null) return;
    stateStore.setQuickMarkDraft(
      _draftForDrag(start: start, end: end, anchor: anchor),
    );
  }

  void _applyStore(QuickMarkStore nextStore, {bool persist = true}) {
    _nextQuickMarkId = nextStore.nextId;
    stateStore.setQuickMarks(nextStore.marks);
    stateStore.setQuickMarkThumbnails(
      QuickMarkThumbnailStore.reconcile(
        marks: nextStore.marks,
        current: thumbnails,
      ),
    );
    fireAndLogFine(
      'hydrate quick mark thumbnails',
      _hydrateThumbnailsFromCatalog(nextStore.marks),
    );
    if (persist) _scheduleSave();
  }
}

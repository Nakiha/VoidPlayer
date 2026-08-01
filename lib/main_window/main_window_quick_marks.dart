import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:flutter/material.dart';
import 'package:path/path.dart' as p;

import '../analysis/analysis_ffi.dart';
import '../app_log.dart';
import '../app_paths.dart';
import '../marks/quick_mark.dart';
import '../marks/quick_mark_export.dart';
import '../marks/quick_mark_media_hash.dart';
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
  int? _pendingJumpMarkId;
  String _lastViewTrace = '';
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

  QuickMarkView get view {
    final currentStore = store;
    final context = frameContext;
    final markView = currentStore.view(
      context: context,
      selectedMarkId: _selectedQuickMarkId,
    );
    _traceView(currentStore, context, markView);
    return markView;
  }

  QuickMarkStore get store =>
      QuickMarkStore(marks: _quickMarks, nextId: _nextQuickMarkId);

  QuickMarkFrameContext get frameContext => QuickMarkFrameContext(
    currentPtsUs: _currentPtsUs,
    presentedFrameAnchors: _presentedFrameAnchors,
    allowTimeFallback: _state.pendingSeekUs == null,
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
    _completePendingJumpIfReady();
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
    if (_playerId == null || trackManager.isEmpty) return;
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
    if (_pendingJumpMarkId == id) {
      _pendingJumpMarkId = null;
    }
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
    if (isVisible(mark)) {
      _pendingJumpMarkId = null;
      stateStore.setSelectedQuickMarkId(id);
      layoutCoordinator.focusQuickMark(mark);
      if (_state.pendingSeekUs != null) {
        playbackCoordinator.seekTo(
          mark.anchor.ptsUs,
          preservePresentedFrameAnchors: true,
        );
      }
      return;
    }
    _pendingJumpMarkId = id;
    // Keep the inspector anchored to the item the user just clicked while the
    // target frame is being sought. The viewport deliberately exposes only a
    // *visible* selected mark, so this does not draw the mark on the stale
    // frame; it only avoids a transient no-selection state and sidebar flash.
    stateStore.setSelectedQuickMarkId(id);
    playbackCoordinator.seekTo(mark.anchor.ptsUs);
  }

  bool isVisible(QuickMark mark) {
    return store.isVisible(mark, frameContext);
  }

  void _completePendingJumpIfReady() {
    final id = _pendingJumpMarkId;
    if (id == null || _state.pendingSeekUs != null) return;
    final mark = store.markById(id);
    if (mark == null) {
      _pendingJumpMarkId = null;
      return;
    }
    if (!isVisible(mark)) return;
    _pendingJumpMarkId = null;
    stateStore.setSelectedQuickMarkId(id);
    layoutCoordinator.focusQuickMark(mark);
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
    _traceAnchor('add-agent-mark', anchor);
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

  int addMetricMarks({
    required int fileId,
    required AnalysisQualityMetric metric,
    required double threshold,
    required AnalysisQualityReport report,
  }) {
    if (!trackManager.entries.any((entry) => entry.fileId == fileId)) {
      return 0;
    }
    final candidates = buildQualityMetricMarks(
      existingMarks: _quickMarks,
      fileId: fileId,
      metric: metric,
      threshold: threshold,
      report: report,
    );
    var nextStore = store;
    for (final candidate in candidates) {
      nextStore = nextStore.add(candidate);
    }
    if (candidates.isNotEmpty) {
      _applyStore(nextStore);
    }
    return candidates.length;
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

  int? get _playerId => _state.playerId;
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
      if (await file.exists()) return computeQuickMarkMediaHash(path);
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
    final anchor = QuickMarkAnchor.fromPresentedFrame(
      fileId: fileId,
      timing: timing,
      fallbackPtsUs: _currentPtsUs,
    );
    if (timing?.isValid == true &&
        mounted() &&
        !shuttingDown() &&
        _state.pendingSeekUs == null &&
        trackManager.entries.any((entry) => entry.fileId == fileId)) {
      // A paused player does not emit another playback-clock event after a
      // mark gesture resolves the displayed frame. Publish the same native
      // frame anchor immediately so a non-zero first-frame PTS remains
      // visible while the global timeline is still at 00:00.000.
      stateStore.setPresentedFrameAnchor(anchor);
    }
    _traceAnchor('anchor-for-file fileId=$fileId', anchor);
    return anchor;
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

  void _traceAnchor(String route, QuickMarkAnchor anchor) {
    if (Platform.environment['VOIDPLAYER_QUICK_MARK_TRACE'] != '1') return;
    log.info('[QuickMarkTrace] $route anchor=${_quickMarkAnchorTrace(anchor)}');
  }

  void _traceView(
    QuickMarkStore currentStore,
    QuickMarkFrameContext context,
    QuickMarkView markView,
  ) {
    if (Platform.environment['VOIDPLAYER_QUICK_MARK_TRACE'] != '1') return;
    final anchors = context.presentedFrameAnchors.entries
        .map((entry) => '${entry.key}:${_quickMarkAnchorTrace(entry.value)}')
        .join(';');
    final marks = currentStore.marks
        .map((mark) {
          final currentAnchor = context.presentedFrameAnchors[mark.fileId];
          final toleranceUs = mark.anchor.durationUs > 0
              ? (mark.anchor.durationUs / 2).round()
              : 0;
          final visible = currentStore.isVisible(mark, context);
          return '#${mark.id}(file=${mark.fileId},visible=$visible,'
              'selected=${mark.id == _selectedQuickMarkId},tol=$toleranceUs,'
              'mark=${_quickMarkAnchorTrace(mark.anchor)},'
              'current=${currentAnchor == null ? "null" : _quickMarkAnchorTrace(currentAnchor)})';
        })
        .join(' | ');
    final signature =
        'clock=${context.currentPtsUs} selected=$_selectedQuickMarkId '
        'visible=${markView.visibleMarkIds.join(",")} '
        'visibleSelected=${markView.visibleSelectedMarkId} '
        'anchors=[$anchors] marks=[$marks]';
    if (signature == _lastViewTrace) return;
    _lastViewTrace = signature;
    log.info('[QuickMarkTrace] view $signature');
  }
}

String _quickMarkAnchorTrace(QuickMarkAnchor anchor) =>
    'pts=${anchor.ptsUs},dts=${anchor.dtsUs},dur=${anchor.durationUs},'
    'afi=${anchor.analysisFrameIndex},mode=${anchor.frameIdentityMode},'
    'spi=${anchor.sourcePacketIndex},sps=${anchor.sourcePacketSize},'
    'spp=${anchor.sourcePacketPos}';

String _qualityMetricDefectType(AnalysisQualityMetric metric) =>
    switch (metric) {
      AnalysisQualityMetric.blockiness => QuickMarkDefectTypes.blocking,
      AnalysisQualityMetric.banding => QuickMarkDefectTypes.banding,
      AnalysisQualityMetric.blur => QuickMarkDefectTypes.blur,
      AnalysisQualityMetric.noise => 'noise',
      AnalysisQualityMetric.flicker => QuickMarkDefectTypes.flicker,
    };

List<QuickMark> buildQualityMetricMarks({
  required List<QuickMark> existingMarks,
  required int fileId,
  required AnalysisQualityMetric metric,
  required double threshold,
  required AnalysisQualityReport report,
}) {
  if (report.hasEventCandidates) {
    return _buildQualityEventMarks(
      existingMarks: existingMarks,
      fileId: fileId,
      metric: metric,
      report: report,
    );
  }
  final existingPtsUs = {
    for (final mark in existingMarks)
      if (mark.fileId == fileId &&
          mark.origin == QuickMarkOrigin.metric &&
          mark.attributes['metric'] == metric.name)
        mark.ptsUs,
  };
  final candidates = <QuickMark>[];
  for (final sample in report.samples) {
    final value = sample.valueFor(metric);
    if (value == null ||
        value < threshold ||
        existingPtsUs.contains(sample.ptsUs)) {
      continue;
    }
    candidates.add(
      QuickMark(
        id: 0,
        anchor: QuickMarkAnchor(
          fileId: fileId,
          ptsUs: sample.ptsUs,
          dtsUs: sample.ptsUs,
        ),
        // Quality proxies are frame-level rather than spatial detectors. Keep
        // the frame outline slightly inside the image so its stroke is not
        // clipped away at the viewport boundary.
        sourceRect: const Rect.fromLTWH(0.015, 0.015, 0.97, 0.97),
        color: const Color(0xFFFFA726),
        text: 'Quality: ${metric.name} ${value.toStringAsFixed(3)}',
        syncAcrossTracks: false,
        origin: QuickMarkOrigin.metric,
        defectType: _qualityMetricDefectType(metric),
        attributes: {
          'metric': metric.name,
          'value': value,
          'threshold': threshold,
          'sampleIndex': sample.sampleIndex,
          'decodedFrameIndex': sample.decodedFrameIndex,
          'schemaVersion': report.schemaVersion,
          'scope': 'frame',
        },
      ),
    );
    existingPtsUs.add(sample.ptsUs);
  }
  return List.unmodifiable(candidates);
}

/// Builds marks from CLI candidate events. The CLI owns thresholding,
/// grouping and spatial matching; the GUI only translates each event of the
/// focused metric into one mark anchored at the event's peak.
///
/// Events with spatial evidence become region marks. For a relative outlier,
/// tile evidence may locate the strongest local response on the event's peak
/// sample. This is visualization evidence, not a replacement for CLI event
/// classification. If tile evidence is unavailable, the event remains a
/// time-only mark and is never painted as a synthetic full-frame rectangle.
List<QuickMark> _buildQualityEventMarks({
  required List<QuickMark> existingMarks,
  required int fileId,
  required AnalysisQualityMetric metric,
  required AnalysisQualityReport report,
}) {
  final resultKey = report.resultKey!;
  final existingEvents = <(String?, String)>{};
  for (final mark in existingMarks) {
    if (mark.fileId != fileId ||
        mark.origin != QuickMarkOrigin.metric ||
        mark.attributes['metric'] != metric.name) {
      continue;
    }
    final eventId = mark.attributes['eventId'];
    if (eventId is String) {
      final existingResultKey = mark.attributes['resultKey'];
      existingEvents.add((
        existingResultKey is String ? existingResultKey : null,
        eventId,
      ));
    }
  }
  final candidates = <QuickMark>[];
  for (final event in report.events) {
    if (event.metric != metric ||
        existingEvents.contains((resultKey, event.eventId))) {
      continue;
    }
    final region = event.region;
    final tileSample = region == null
        ? report.tileSampleAt(event.peakSampleIndex)
        : null;
    final tilePeak = tileSample?.strongestTileFor(metric);
    final scope = region != null
        ? 'region'
        : (tilePeak != null ? 'tile' : 'time');
    final left = (region?.x ?? tilePeak?.x ?? 0.0).clamp(0.0, 1.0);
    final top = (region?.y ?? tilePeak?.y ?? 0.0).clamp(0.0, 1.0);
    final width = region?.width ?? tilePeak?.width;
    final height = region?.height ?? tilePeak?.height;
    final value = region?.score ?? event.peakScore;
    candidates.add(
      QuickMark(
        id: 0,
        anchor: QuickMarkAnchor(
          fileId: fileId,
          ptsUs: event.peakPtsUs,
          dtsUs: event.peakPtsUs,
        ),
        sourceRect: width == null || height == null
            // Time-only marks are not painted; the inset frame rect only
            // seeds the sidebar thumbnail crop.
            ? const Rect.fromLTWH(0.015, 0.015, 0.97, 0.97)
            : Rect.fromLTWH(
                left,
                top,
                width.clamp(0.0, 1.0 - left),
                height.clamp(0.0, 1.0 - top),
              ),
        color: const Color(0xFFFFA726),
        text: 'Quality: ${metric.name} ${value.toStringAsFixed(3)}',
        syncAcrossTracks: false,
        origin: QuickMarkOrigin.metric,
        defectType: _qualityMetricDefectType(metric),
        attributes: {
          'metric': metric.name,
          'value': value,
          'threshold': event.threshold,
          'eventId': event.eventId,
          'resultKey': resultKey,
          'classification': event.classification.name,
          'startPtsUs': event.startPtsUs,
          'endPtsUs': event.endPtsUs,
          'schemaVersion': report.schemaVersion,
          'scope': scope,
          if (tilePeak != null) ...{
            'tileMetricVersion': tileSample!.tileMetricVersion,
            'tileAlgorithm': tileSample.metrics[metric]!.algorithm,
            'tileColumn': tilePeak.column,
            'tileRow': tilePeak.row,
            'tileValue': tilePeak.value,
          },
        },
      ),
    );
    existingEvents.add((resultKey, event.eventId));
  }
  return List.unmodifiable(candidates);
}

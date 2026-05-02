// ignore_for_file: unused_field

import 'dart:ffi';
import 'package:ffi/ffi.dart';

// ===========================================================================
// FFI Struct definitions — mirror C++ structs in video_renderer_plugin.h
// ===========================================================================

final class NakiAnalysisSummary extends Struct {
  @Int32()
  external int loaded;
  @Int32()
  external int frameCount;
  @Int32()
  external int packetCount;
  @Int32()
  external int naluCount;
  @Int32()
  external int videoWidth;
  @Int32()
  external int videoHeight;
  @Int32()
  external int timeBaseNum;
  @Int32()
  external int timeBaseDen;
  @Int32()
  external int currentFrameIdx;
  @Int32()
  external int codec;

  // Reserved native ABI padding.
  @Array(6)
  external Array<Int32> _reserved;
}

final class NakiFrameInfo extends Struct {
  @Int32()
  external int poc;
  @Int32()
  external int temporalId;
  @Int32()
  external int sliceType;
  @Int32()
  external int nalType;
  @Int32()
  external int avgQp;
  @Int32()
  external int numRefL0;
  @Int32()
  external int numRefL1;

  @Array(15)
  external Array<Int32> refPocsL0;
  @Array(15)
  external Array<Int32> refPocsL1;

  @Int64()
  external int pts;
  @Int64()
  external int dts;
  @Int32()
  external int packetSize;
  @Int32()
  external int keyframe;

  // Reserved native ABI padding.
  @Array(2)
  external Array<Int32> _reserved;
}

final class NakiNaluInfo extends Struct {
  @Uint64()
  external int offset;
  @Uint32()
  external int size;
  @Uint8()
  external int nalType;
  @Uint8()
  external int temporalId;
  @Uint8()
  external int layerId;
  @Uint8()
  external int flags;
}

final class NakiFrameBucket extends Struct {
  @Int32()
  external int startFrame;
  @Int32()
  external int frameCount;
  @Int32()
  external int packetSizeMin;
  @Int32()
  external int packetSizeMax;
  @Int64()
  external int packetSizeSum;
  @Int32()
  external int qpMin;
  @Int32()
  external int qpMax;
  @Int64()
  external int qpSum;
  @Int32()
  external int keyframeCount;

  @Array(3)
  external Array<Int32> _reserved;
}

final class NakiOverlayState extends Struct {
  @Int32()
  external int showCuGrid;
  @Int32()
  external int showPredMode;
  @Int32()
  external int showQpHeatmap;
  @Int32()
  // Reserved native ABI padding.
  external int _reserved;
}

// ===========================================================================
// FFI function typedefs
// ===========================================================================

typedef _LoadNative =
    Int32 Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);
typedef _LoadDart = int Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);

typedef _UnloadNative = Void Function();
typedef _UnloadDart = void Function();

typedef _GetSummaryNative = Pointer<NakiAnalysisSummary> Function();
typedef _GetSummaryDart = Pointer<NakiAnalysisSummary> Function();

typedef _GetFramesRangeNative =
    Int32 Function(Int32, Pointer<NakiFrameInfo>, Int32);
typedef _GetFramesRangeDart = int Function(int, Pointer<NakiFrameInfo>, int);

typedef _GetNalusRangeNative =
    Int32 Function(Int32, Pointer<NakiNaluInfo>, Int32);
typedef _GetNalusRangeDart = int Function(int, Pointer<NakiNaluInfo>, int);

typedef _IndexMapNative = Int32 Function(Int32);
typedef _IndexMapDart = int Function(int);

typedef _GetFrameBucketsNative =
    Int32 Function(Int32, Int32, Pointer<NakiFrameBucket>, Int32);
typedef _GetFrameBucketsDart =
    int Function(int, int, Pointer<NakiFrameBucket>, int);

typedef _SetOverlayNative = Void Function(Pointer<NakiOverlayState>);
typedef _SetOverlayDart = void Function(Pointer<NakiOverlayState>);

typedef _GenerateNative = Int32 Function(Pointer<Utf8>, Pointer<Utf8>);
typedef _GenerateDart = int Function(Pointer<Utf8>, Pointer<Utf8>);

typedef _OpenNative =
    Pointer<Void> Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);
typedef _OpenDart =
    Pointer<Void> Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);

typedef _CloseNative = Void Function(Pointer<Void>);
typedef _CloseDart = void Function(Pointer<Void>);

typedef _HandleGetSummaryNative =
    Pointer<NakiAnalysisSummary> Function(Pointer<Void>);
typedef _HandleGetSummaryDart =
    Pointer<NakiAnalysisSummary> Function(Pointer<Void>);

typedef _HandleGetFramesRangeNative =
    Int32 Function(Pointer<Void>, Int32, Pointer<NakiFrameInfo>, Int32);
typedef _HandleGetFramesRangeDart =
    int Function(Pointer<Void>, int, Pointer<NakiFrameInfo>, int);

typedef _HandleGetNalusRangeNative =
    Int32 Function(Pointer<Void>, Int32, Pointer<NakiNaluInfo>, Int32);
typedef _HandleGetNalusRangeDart =
    int Function(Pointer<Void>, int, Pointer<NakiNaluInfo>, int);

typedef _HandleIndexMapNative = Int32 Function(Pointer<Void>, Int32);
typedef _HandleIndexMapDart = int Function(Pointer<Void>, int);

typedef _HandleGetFrameBucketsNative =
    Int32 Function(
      Pointer<Void>,
      Int32,
      Int32,
      Pointer<NakiFrameBucket>,
      Int32,
    );
typedef _HandleGetFrameBucketsDart =
    int Function(Pointer<Void>, int, int, Pointer<NakiFrameBucket>, int);

// ===========================================================================
// Native symbol lookup
// ===========================================================================

final _dl = DynamicLibrary.executable();

final _load = _dl.lookupFunction<_LoadNative, _LoadDart>('naki_analysis_load');
final _unload = _dl.lookupFunction<_UnloadNative, _UnloadDart>(
  'naki_analysis_unload',
);
final _getSummary = _dl.lookupFunction<_GetSummaryNative, _GetSummaryDart>(
  'naki_analysis_get_summary',
);
final _getFramesRange = _dl
    .lookupFunction<_GetFramesRangeNative, _GetFramesRangeDart>(
      'naki_analysis_get_frames_range',
    );
final _getNalusRange = _dl
    .lookupFunction<_GetNalusRangeNative, _GetNalusRangeDart>(
      'naki_analysis_get_nalus_range',
    );
final _frameToNalu = _dl.lookupFunction<_IndexMapNative, _IndexMapDart>(
  'naki_analysis_frame_to_nalu',
);
final _naluToFrame = _dl.lookupFunction<_IndexMapNative, _IndexMapDart>(
  'naki_analysis_nalu_to_frame',
);
final _getFrameBuckets = _dl
    .lookupFunction<_GetFrameBucketsNative, _GetFrameBucketsDart>(
      'naki_analysis_get_frame_buckets',
    );
final _setOverlay = _dl.lookupFunction<_SetOverlayNative, _SetOverlayDart>(
  'naki_analysis_set_overlay',
);
final _generate = _dl.lookupFunction<_GenerateNative, _GenerateDart>(
  'naki_analysis_generate',
);
final _open = _dl.lookupFunction<_OpenNative, _OpenDart>('naki_analysis_open');
final _close = _dl.lookupFunction<_CloseNative, _CloseDart>(
  'naki_analysis_close',
);
final _handleGetSummary = _dl
    .lookupFunction<_HandleGetSummaryNative, _HandleGetSummaryDart>(
      'naki_analysis_handle_get_summary',
    );
final _handleGetFramesRange = _dl
    .lookupFunction<_HandleGetFramesRangeNative, _HandleGetFramesRangeDart>(
      'naki_analysis_handle_get_frames_range',
    );
final _handleGetNalusRange = _dl
    .lookupFunction<_HandleGetNalusRangeNative, _HandleGetNalusRangeDart>(
      'naki_analysis_handle_get_nalus_range',
    );
final _handleFrameToNalu = _dl
    .lookupFunction<_HandleIndexMapNative, _HandleIndexMapDart>(
      'naki_analysis_handle_frame_to_nalu',
    );
final _handleNaluToFrame = _dl
    .lookupFunction<_HandleIndexMapNative, _HandleIndexMapDart>(
      'naki_analysis_handle_nalu_to_frame',
    );
final _handleGetFrameBuckets = _dl
    .lookupFunction<_HandleGetFrameBucketsNative, _HandleGetFrameBucketsDart>(
      'naki_analysis_handle_get_frame_buckets',
    );

// ===========================================================================
// Pure Dart data classes — copies of FFI struct fields, safe after free
// ===========================================================================

class FrameInfo {
  final int poc;
  final int temporalId;
  final int sliceType;
  final int nalType;
  final int avgQp;
  final int numRefL0;
  final int numRefL1;
  final List<int> refPocsL0;
  final List<int> refPocsL1;
  final int pts;
  final int dts;
  final int packetSize;
  final int keyframe;

  FrameInfo({
    required this.poc,
    required this.temporalId,
    required this.sliceType,
    required this.nalType,
    required this.avgQp,
    required this.numRefL0,
    required this.numRefL1,
    required this.refPocsL0,
    required this.refPocsL1,
    required this.pts,
    required this.dts,
    required this.packetSize,
    required this.keyframe,
  });
}

class NaluInfo {
  final int offset;
  final int size;
  final int nalType;
  final int temporalId;
  final int layerId;
  final int flags;

  NaluInfo({
    required this.offset,
    required this.size,
    required this.nalType,
    required this.temporalId,
    required this.layerId,
    required this.flags,
  });
}

class FrameBucket {
  final int startFrame;
  final int frameCount;
  final int packetSizeMin;
  final int packetSizeMax;
  final int packetSizeSum;
  final int qpMin;
  final int qpMax;
  final int qpSum;
  final int keyframeCount;

  FrameBucket({
    required this.startFrame,
    required this.frameCount,
    required this.packetSizeMin,
    required this.packetSizeMax,
    required this.packetSizeSum,
    required this.qpMin,
    required this.qpMax,
    required this.qpSum,
    required this.keyframeCount,
  });

  double get avgPacketSize => frameCount == 0 ? 0 : packetSizeSum / frameCount;
  double get avgQp => frameCount == 0 ? 0 : qpSum / frameCount;
}

class AnalysisSummary {
  final int loaded;
  final int frameCount;
  final int packetCount;
  final int naluCount;
  final int videoWidth;
  final int videoHeight;
  final int timeBaseNum;
  final int timeBaseDen;
  final int currentFrameIdx;
  final int codec;

  const AnalysisSummary({
    required this.loaded,
    required this.frameCount,
    required this.packetCount,
    required this.naluCount,
    required this.videoWidth,
    required this.videoHeight,
    required this.timeBaseNum,
    required this.timeBaseDen,
    required this.currentFrameIdx,
    required this.codec,
  });

  factory AnalysisSummary.fromNative(NakiAnalysisSummary s) => AnalysisSummary(
    loaded: s.loaded,
    frameCount: s.frameCount,
    packetCount: s.packetCount,
    naluCount: s.naluCount,
    videoWidth: s.videoWidth,
    videoHeight: s.videoHeight,
    timeBaseNum: s.timeBaseNum,
    timeBaseDen: s.timeBaseDen,
    currentFrameIdx: s.currentFrameIdx,
    codec: s.codec,
  );
}

const _emptySummary = AnalysisSummary(
  loaded: 0,
  frameCount: 0,
  packetCount: 0,
  naluCount: 0,
  videoWidth: 0,
  videoHeight: 0,
  timeBaseNum: 0,
  timeBaseDen: 0,
  currentFrameIdx: -1,
  codec: 0,
);

FrameInfo _frameInfoAt(Pointer<NakiFrameInfo> ptr, int i) {
  final f = ptr[i];
  return FrameInfo(
    poc: f.poc,
    temporalId: f.temporalId,
    sliceType: f.sliceType,
    nalType: f.nalType,
    avgQp: f.avgQp,
    numRefL0: f.numRefL0,
    numRefL1: f.numRefL1,
    refPocsL0: List.generate(15, (j) => f.refPocsL0[j]),
    refPocsL1: List.generate(15, (j) => f.refPocsL1[j]),
    pts: f.pts,
    dts: f.dts,
    packetSize: f.packetSize,
    keyframe: f.keyframe,
  );
}

NaluInfo _naluInfoAt(Pointer<NakiNaluInfo> ptr, int i) {
  final n = ptr[i];
  return NaluInfo(
    offset: n.offset,
    size: n.size,
    nalType: n.nalType,
    temporalId: n.temporalId,
    layerId: n.layerId,
    flags: n.flags,
  );
}

FrameBucket _frameBucketAt(Pointer<NakiFrameBucket> ptr, int i) {
  final b = ptr[i];
  return FrameBucket(
    startFrame: b.startFrame,
    frameCount: b.frameCount,
    packetSizeMin: b.packetSizeMin,
    packetSizeMax: b.packetSizeMax,
    packetSizeSum: b.packetSizeSum,
    qpMin: b.qpMin,
    qpMax: b.qpMax,
    qpSum: b.qpSum,
    keyframeCount: b.keyframeCount,
  );
}

class AnalysisSession {
  Pointer<Void> _handle;

  AnalysisSession._(this._handle);

  static AnalysisSession? open(
    String vbs2Path,
    String vbiPath,
    String vbtPath,
  ) {
    final vbs2 = vbs2Path.toNativeUtf8(allocator: calloc);
    final vbi = vbiPath.toNativeUtf8(allocator: calloc);
    final vbt = vbtPath.toNativeUtf8(allocator: calloc);
    try {
      final handle = _open(vbs2, vbi, vbt);
      if (handle == nullptr) return null;
      return AnalysisSession._(handle);
    } finally {
      calloc.free(vbs2);
      calloc.free(vbi);
      calloc.free(vbt);
    }
  }

  bool get isOpen => _handle != nullptr;

  void close() {
    if (_handle == nullptr) return;
    _close(_handle);
    _handle = nullptr;
  }

  AnalysisSummary get summary {
    if (_handle == nullptr) return _emptySummary;
    final ptr = _handleGetSummary(_handle);
    if (ptr == nullptr) return _emptySummary;
    return AnalysisSummary.fromNative(ptr.ref);
  }

  List<FrameInfo> get frames {
    final s = summary;
    return framesRange(0, s.loaded == 0 ? 0 : s.frameCount);
  }

  List<FrameInfo> framesRange(int start, int count) {
    final s = summary;
    if (s.loaded == 0 || s.frameCount == 0 || _handle == nullptr) return [];
    if (start < 0 || count <= 0 || start >= s.frameCount) return [];
    final safeCount = count.clamp(0, s.frameCount - start).toInt();
    if (safeCount <= 0) return [];
    final ptr = calloc<NakiFrameInfo>(safeCount);
    try {
      final actual = _handleGetFramesRange(
        _handle,
        start,
        ptr,
        safeCount,
      ).clamp(0, safeCount).toInt();
      return List.generate(actual, (i) => _frameInfoAt(ptr, i));
    } finally {
      calloc.free(ptr);
    }
  }

  List<NaluInfo> get nalus {
    final s = summary;
    return nalusRange(0, s.loaded == 0 ? 0 : s.naluCount);
  }

  List<NaluInfo> nalusRange(int start, int count) {
    final s = summary;
    if (s.loaded == 0 || s.naluCount == 0 || _handle == nullptr) return [];
    if (start < 0 || count <= 0 || start >= s.naluCount) return [];
    final safeCount = count.clamp(0, s.naluCount - start).toInt();
    if (safeCount <= 0) return [];
    final ptr = calloc<NakiNaluInfo>(safeCount);
    try {
      final actual = _handleGetNalusRange(
        _handle,
        start,
        ptr,
        safeCount,
      ).clamp(0, safeCount).toInt();
      return List.generate(actual, (i) => _naluInfoAt(ptr, i));
    } finally {
      calloc.free(ptr);
    }
  }

  int frameToNalu(int frameIndex) {
    if (_handle == nullptr || frameIndex < 0) return -1;
    return _handleFrameToNalu(_handle, frameIndex);
  }

  int naluToFrame(int naluIndex) {
    if (_handle == nullptr || naluIndex < 0) return -1;
    return _handleNaluToFrame(_handle, naluIndex);
  }

  List<FrameBucket> frameBuckets({
    required int start,
    required int bucketSize,
    required int maxCount,
  }) {
    final s = summary;
    if (s.loaded == 0 || s.frameCount == 0 || _handle == nullptr) return [];
    if (start < 0 ||
        bucketSize <= 0 ||
        maxCount <= 0 ||
        start >= s.frameCount) {
      return [];
    }
    final ptr = calloc<NakiFrameBucket>(maxCount);
    try {
      final actual = _handleGetFrameBuckets(
        _handle,
        start,
        bucketSize,
        ptr,
        maxCount,
      ).clamp(0, maxCount).toInt();
      return List.generate(actual, (i) => _frameBucketAt(ptr, i));
    } finally {
      calloc.free(ptr);
    }
  }
}

// ===========================================================================
// High-level API
// ===========================================================================

class AnalysisFfi {
  /// Load analysis files from specific paths.
  /// Returns true on success.
  static bool load(String vbs2Path, String vbiPath, String vbtPath) {
    final vbs2 = vbs2Path.toNativeUtf8(allocator: calloc);
    final vbi = vbiPath.toNativeUtf8(allocator: calloc);
    final vbt = vbtPath.toNativeUtf8(allocator: calloc);
    try {
      return _load(vbs2, vbi, vbt) != 0;
    } finally {
      calloc.free(vbs2);
      calloc.free(vbi);
      calloc.free(vbt);
    }
  }

  /// Unload analysis data.
  static void unload() => _unload();

  /// Get analysis summary snapshot.
  static AnalysisSummary get summary {
    final ptr = _getSummary();
    if (ptr == nullptr) return _emptySummary;
    return AnalysisSummary.fromNative(ptr.ref);
  }

  /// Read all frame info into a Dart list.
  /// Returns plain Dart objects (safe after the FFI buffer is freed).
  static List<FrameInfo> get frames {
    final s = summary;
    return framesRange(0, s.loaded == 0 ? 0 : s.frameCount);
  }

  /// Read a bounded frame range into Dart objects.
  static List<FrameInfo> framesRange(int start, int count) {
    final s = summary;
    if (s.loaded == 0 || s.frameCount == 0) return [];
    if (start < 0 || count <= 0 || start >= s.frameCount) return [];
    final safeCount = count.clamp(0, s.frameCount - start).toInt();
    if (safeCount <= 0) return [];
    final ptr = calloc<NakiFrameInfo>(safeCount);
    try {
      final actual = _getFramesRange(
        start,
        ptr,
        safeCount,
      ).clamp(0, safeCount).toInt();
      return List.generate(actual, (i) => _frameInfoAt(ptr, i));
    } finally {
      calloc.free(ptr);
    }
  }

  /// Read all NALU info into a Dart list.
  /// Returns plain Dart objects (safe after the FFI buffer is freed).
  static List<NaluInfo> get nalus {
    final s = summary;
    return nalusRange(0, s.loaded == 0 ? 0 : s.naluCount);
  }

  /// Read a bounded NALU range into Dart objects.
  static List<NaluInfo> nalusRange(int start, int count) {
    final s = summary;
    if (s.loaded == 0 || s.naluCount == 0) return [];
    if (start < 0 || count <= 0 || start >= s.naluCount) return [];
    final safeCount = count.clamp(0, s.naluCount - start).toInt();
    if (safeCount <= 0) return [];
    final ptr = calloc<NakiNaluInfo>(safeCount);
    try {
      final actual = _getNalusRange(
        start,
        ptr,
        safeCount,
      ).clamp(0, safeCount).toInt();
      return List.generate(actual, (i) => _naluInfoAt(ptr, i));
    } finally {
      calloc.free(ptr);
    }
  }

  static int frameToNalu(int frameIndex) {
    if (frameIndex < 0) return -1;
    return _frameToNalu(frameIndex);
  }

  static int naluToFrame(int naluIndex) {
    if (naluIndex < 0) return -1;
    return _naluToFrame(naluIndex);
  }

  static List<FrameBucket> frameBuckets({
    required int start,
    required int bucketSize,
    required int maxCount,
  }) {
    final s = summary;
    if (s.loaded == 0 || s.frameCount == 0) return [];
    if (start < 0 ||
        bucketSize <= 0 ||
        maxCount <= 0 ||
        start >= s.frameCount) {
      return [];
    }
    final ptr = calloc<NakiFrameBucket>(maxCount);
    try {
      final actual = _getFrameBuckets(
        start,
        bucketSize,
        ptr,
        maxCount,
      ).clamp(0, maxCount).toInt();
      return List.generate(actual, (i) => _frameBucketAt(ptr, i));
    } finally {
      calloc.free(ptr);
    }
  }

  /// Set overlay visibility flags.
  static void setOverlay({
    required bool showCuGrid,
    required bool showPredMode,
    required bool showQpHeatmap,
  }) {
    final state = calloc<NakiOverlayState>();
    try {
      state.ref.showCuGrid = showCuGrid ? 1 : 0;
      state.ref.showPredMode = showPredMode ? 1 : 0;
      state.ref.showQpHeatmap = showQpHeatmap ? 1 : 0;
      _setOverlay(state);
    } finally {
      calloc.free(state);
    }
  }

  /// Generate analysis files (VBI + VBT) for a video.
  /// [videoPath] is the source video file.
  /// [hash] is used as the base name for output files (data/{hash}.vbi etc.)
  /// Returns true on success.
  static bool generateAnalysis(String videoPath, String hash) {
    final video = videoPath.toNativeUtf8(allocator: calloc);
    final hashStr = hash.toNativeUtf8(allocator: calloc);
    try {
      return _generate(video, hashStr) != 0;
    } finally {
      calloc.free(video);
      calloc.free(hashStr);
    }
  }
}

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

typedef _GetFramesNative = Int32 Function(Pointer<NakiFrameInfo>, Int32);
typedef _GetFramesDart = int Function(Pointer<NakiFrameInfo>, int);

typedef _GetNalusNative = Int32 Function(Pointer<NakiNaluInfo>, Int32);
typedef _GetNalusDart = int Function(Pointer<NakiNaluInfo>, int);

typedef _SetOverlayNative = Void Function(Pointer<NakiOverlayState>);
typedef _SetOverlayDart = void Function(Pointer<NakiOverlayState>);

typedef _GenerateNative = Int32 Function(Pointer<Utf8>, Pointer<Utf8>);
typedef _GenerateDart = int Function(Pointer<Utf8>, Pointer<Utf8>);

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
final _getFrames = _dl.lookupFunction<_GetFramesNative, _GetFramesDart>(
  'naki_analysis_get_frames',
);
final _getNalus = _dl.lookupFunction<_GetNalusNative, _GetNalusDart>(
  'naki_analysis_get_nalus',
);
final _setOverlay = _dl.lookupFunction<_SetOverlayNative, _SetOverlayDart>(
  'naki_analysis_set_overlay',
);
final _generate = _dl.lookupFunction<_GenerateNative, _GenerateDart>(
  'naki_analysis_generate',
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
  static AnalysisSummary get summary =>
      AnalysisSummary.fromNative(_getSummary().ref);

  /// Read all frame info into a Dart list.
  /// Returns plain Dart objects (safe after the FFI buffer is freed).
  static List<FrameInfo> get frames {
    final s = summary;
    if (s.loaded == 0) return [];
    final count = s.frameCount;
    if (count == 0) return [];
    final ptr = calloc<NakiFrameInfo>(count);
    try {
      final actual = _getFrames(ptr, count);
      return List.generate(actual, (i) {
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
      });
    } finally {
      calloc.free(ptr);
    }
  }

  /// Read all NALU info into a Dart list.
  /// Returns plain Dart objects (safe after the FFI buffer is freed).
  static List<NaluInfo> get nalus {
    final s = summary;
    if (s.loaded == 0) return [];
    final count = s.naluCount;
    if (count == 0) return [];
    final ptr = calloc<NakiNaluInfo>(count);
    try {
      final actual = _getNalus(ptr, count);
      return List.generate(actual, (i) {
        final n = ptr[i];
        return NaluInfo(
          offset: n.offset,
          size: n.size,
          nalType: n.nalType,
          temporalId: n.temporalId,
          layerId: n.layerId,
          flags: n.flags,
        );
      });
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

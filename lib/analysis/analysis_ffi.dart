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

  @Array(7)
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
  external int _reserved;
}

// ===========================================================================
// FFI function typedefs
// ===========================================================================

typedef _LoadNative = Int32 Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);
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

// ===========================================================================
// Native symbol lookup
// ===========================================================================

final _dl = DynamicLibrary.executable();

final _load = _dl.lookupFunction<_LoadNative, _LoadDart>('naki_analysis_load');
final _unload = _dl.lookupFunction<_UnloadNative, _UnloadDart>('naki_analysis_unload');
final _getSummary =
    _dl.lookupFunction<_GetSummaryNative, _GetSummaryDart>('naki_analysis_get_summary');
final _getFrames =
    _dl.lookupFunction<_GetFramesNative, _GetFramesDart>('naki_analysis_get_frames');
final _getNalus =
    _dl.lookupFunction<_GetNalusNative, _GetNalusDart>('naki_analysis_get_nalus');
final _setOverlay =
    _dl.lookupFunction<_SetOverlayNative, _SetOverlayDart>('naki_analysis_set_overlay');

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
  static NakiAnalysisSummary get summary => _getSummary().ref;

  /// Read all frame info into a Dart list.
  static List<NakiFrameInfo> get frames {
    final s = summary;
    if (s.loaded == 0) return [];
    final count = s.frameCount;
    final ptr = calloc<NakiFrameInfo>(count);
    try {
      final actual = _getFrames(ptr, count);
      return List.generate(actual, (i) => ptr[i]);
    } finally {
      calloc.free(ptr);
    }
  }

  /// Read all NALU info into a Dart list.
  static List<NakiNaluInfo> get nalus {
    final s = summary;
    if (s.loaded == 0) return [];
    final count = s.naluCount;
    final ptr = calloc<NakiNaluInfo>(count);
    try {
      final actual = _getNalus(ptr, count);
      return List.generate(actual, (i) => ptr[i]);
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
}

// ignore_for_file: unused_field

import 'dart:ffi';
import 'dart:io';
import 'package:ffi/ffi.dart';

import '../app_paths.dart';
import '../utils/file_lock.dart';
import 'analysis_cache.dart';

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
  external int showPredLines;
  @Int32()
  external int showCuBitCostHeatmap;
  @Int32()
  external int opacityPermille;
  @Int32()
  external int mode;
  @Int32()
  external int trackFileId;
  @Int32()
  external int _reserved;
}

final class NakiAnalysisGenerationJobResult extends Struct {
  @Uint64()
  external int jobId;
  @Int32()
  external int kind;
  @Int32()
  external int ok;
  @Int32()
  external int status;
  @Int32()
  external int startFrame;
  @Int32()
  external int endFrame;
  @Int32()
  external int priority;
  @Int32()
  external int _reserved;
  @Array(65)
  external Array<Int8> hash;
  @Array(256)
  external Array<Int8> message;
}

final class NakiAnalysisGenerationServiceStats extends Struct {
  @Int32()
  external int workerCount;
  @Int32()
  external int activeWorkers;
  @Int32()
  external int pendingJobs;
  @Int32()
  external int runningJobs;
  @Int32()
  external int completedJobs;
  @Int32()
  external int failedJobs;
  @Int32()
  external int dedupedJobs;
  @Int32()
  external int backpressureDropCount;
  @Uint64()
  external int submittedJobs;
}

final class NakiQualityDistribution extends Struct {
  @Uint64()
  external int count;
  @Double()
  external double mean;
  @Double()
  external double p95;
  @Double()
  external double maximum;
}

final class NakiQualitySummary extends Struct {
  @Uint32()
  external int schemaVersion;
  @Int32()
  external int videoWidth;
  @Int32()
  external int videoHeight;
  @Int32()
  external int bitDepth;
  @Int64()
  external int sampleIntervalUs;
  @Uint32()
  external int maxSamples;
  @Int32()
  external int truncated;
  @Uint64()
  external int unsupportedPixelFrames;
  @Uint64()
  external int sampleCount;
  external NakiQualityDistribution blockiness;
  external NakiQualityDistribution banding;
  external NakiQualityDistribution blur;
  external NakiQualityDistribution noise;
  external NakiQualityDistribution flicker;
}

final class NakiQualitySample extends Struct {
  @Uint64()
  external int sampleIndex;
  @Uint64()
  external int decodedFrameIndex;
  @Int64()
  external int ptsUs;
  @Double()
  external double blockiness;
  @Double()
  external double banding;
  @Double()
  external double blur;
  @Double()
  external double noise;
  @Double()
  external double flicker;
  @Double()
  external double averageQp;
}

// ===========================================================================
// FFI function typedefs
// ===========================================================================

typedef _SetOverlayNative = Void Function(Pointer<NakiOverlayState>);
typedef _SetOverlayDart = void Function(Pointer<NakiOverlayState>);

typedef _SetOverlayTrackNative = Int32 Function(Int32, Pointer<Utf8>);
typedef _SetOverlayTrackDart = int Function(int, Pointer<Utf8>);

typedef _ClearOverlayTracksNative = Void Function();
typedef _ClearOverlayTracksDart = void Function();

typedef _GenerateVac2BaseNative =
    Int32 Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>, Int64);
typedef _GenerateVac2BaseDart =
    int Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>, int);

typedef _GenerateVac2OverlayChunkNative =
    Int32 Function(
      Pointer<Utf8>,
      Pointer<Utf8>,
      Pointer<Utf8>,
      Int32,
      Int32,
      Int64,
    );
typedef _GenerateVac2OverlayChunkDart =
    int Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>, int, int, int);

typedef _SubmitVac2OverlayChunkNative =
    Uint64 Function(
      Pointer<Utf8>,
      Pointer<Utf8>,
      Pointer<Utf8>,
      Int32,
      Int32,
      Int64,
      Int32,
    );
typedef _SubmitVac2OverlayChunkDart =
    int Function(
      Pointer<Utf8>,
      Pointer<Utf8>,
      Pointer<Utf8>,
      int,
      int,
      int,
      int,
    );

typedef _PollGenerationJobsNative =
    Int32 Function(Pointer<NakiAnalysisGenerationJobResult>, Int32);
typedef _PollGenerationJobsDart =
    int Function(Pointer<NakiAnalysisGenerationJobResult>, int);

typedef _GetGenerationServiceStatsNative =
    Void Function(Pointer<NakiAnalysisGenerationServiceStats>);
typedef _GetGenerationServiceStatsDart =
    void Function(Pointer<NakiAnalysisGenerationServiceStats>);

typedef _OpenNative = Pointer<Void> Function(Pointer<Utf8>);
typedef _OpenDart = Pointer<Void> Function(Pointer<Utf8>);

typedef _CloseNative = Void Function(Pointer<Void>);
typedef _CloseDart = void Function(Pointer<Void>);

typedef _HandleGetSummaryNative =
    Pointer<NakiAnalysisSummary> Function(Pointer<Void>);
typedef _HandleGetSummaryDart =
    Pointer<NakiAnalysisSummary> Function(Pointer<Void>);

typedef _HandleFrameIndexForTimestampNative =
    Int32 Function(Pointer<Void>, Int64, Int64);
typedef _HandleFrameIndexForTimestampDart =
    int Function(Pointer<Void>, int, int);

typedef _HandleFrameIndexForSourcePacketNative =
    Int32 Function(Pointer<Void>, Int64, Int32, Int32, Int64, Int64);
typedef _HandleFrameIndexForSourcePacketDart =
    int Function(Pointer<Void>, int, int, int, int, int);

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

typedef _AbiIntNative = Int32 Function();
typedef _AbiIntDart = int Function();

typedef _LastErrorNative = Int32 Function(Pointer<Int8>, Int32);
typedef _LastErrorDart = int Function(Pointer<Int8>, int);

typedef _QualityAnalyzeNative =
    Pointer<Void> Function(Pointer<Utf8>, Int64, Uint32);
typedef _QualityAnalyzeDart = Pointer<Void> Function(Pointer<Utf8>, int, int);

typedef _QualityCloseNative = Void Function(Pointer<Void>);
typedef _QualityCloseDart = void Function(Pointer<Void>);

typedef _QualityGetSummaryNative =
    Int32 Function(Pointer<Void>, Pointer<NakiQualitySummary>);
typedef _QualityGetSummaryDart =
    int Function(Pointer<Void>, Pointer<NakiQualitySummary>);

typedef _QualityGetSamplesNative =
    Int32 Function(Pointer<Void>, Int32, Pointer<NakiQualitySample>, Int32);
typedef _QualityGetSamplesDart =
    int Function(Pointer<Void>, int, Pointer<NakiQualitySample>, int);

// ===========================================================================
// Native symbol lookup
// ===========================================================================

const int _expectedAnalysisAbiVersion = 1;

class AnalysisFfiUnavailable implements Exception {
  final String message;
  final Object? cause;

  const AnalysisFfiUnavailable(this.message, [this.cause]);

  @override
  String toString() => cause == null
      ? 'AnalysisFfiUnavailable: $message'
      : 'AnalysisFfiUnavailable: $message ($cause)';
}

class _AnalysisNativeBindings {
  _AnalysisNativeBindings._(DynamicLibrary library) {
    abiVersion = library.lookupFunction<_AbiIntNative, _AbiIntDart>(
      'naki_analysis_abi_version',
    );
    sizeofSummary = library.lookupFunction<_AbiIntNative, _AbiIntDart>(
      'naki_analysis_sizeof_summary',
    );
    sizeofFrameInfo = library.lookupFunction<_AbiIntNative, _AbiIntDart>(
      'naki_analysis_sizeof_frame_info',
    );
    sizeofNaluInfo = library.lookupFunction<_AbiIntNative, _AbiIntDart>(
      'naki_analysis_sizeof_nalu_info',
    );
    sizeofFrameBucket = library.lookupFunction<_AbiIntNative, _AbiIntDart>(
      'naki_analysis_sizeof_frame_bucket',
    );
    sizeofOverlayState = library.lookupFunction<_AbiIntNative, _AbiIntDart>(
      'naki_analysis_sizeof_overlay_state',
    );
    lastError = library.lookupFunction<_LastErrorNative, _LastErrorDart>(
      'naki_analysis_last_error',
    );

    setOverlay = library.lookupFunction<_SetOverlayNative, _SetOverlayDart>(
      'naki_analysis_set_overlay',
    );
    setOverlayTrack = library
        .lookupFunction<_SetOverlayTrackNative, _SetOverlayTrackDart>(
          'naki_analysis_set_overlay_track',
        );
    clearOverlayTracks = library
        .lookupFunction<_ClearOverlayTracksNative, _ClearOverlayTracksDart>(
          'naki_analysis_clear_overlay_tracks',
        );
    generateVac2Base = library
        .lookupFunction<_GenerateVac2BaseNative, _GenerateVac2BaseDart>(
          'naki_analysis_generate_vac2_base',
        );
    generateVac2OverlayChunk = library
        .lookupFunction<
          _GenerateVac2OverlayChunkNative,
          _GenerateVac2OverlayChunkDart
        >('naki_analysis_generate_vac2_overlay_chunk');
    try {
      submitVac2OverlayChunk = library
          .lookupFunction<
            _SubmitVac2OverlayChunkNative,
            _SubmitVac2OverlayChunkDart
          >('naki_analysis_submit_vac2_overlay_chunk');
      pollGenerationJobs = library
          .lookupFunction<_PollGenerationJobsNative, _PollGenerationJobsDart>(
            'naki_analysis_poll_generation_jobs',
          );
      getGenerationServiceStats = library
          .lookupFunction<
            _GetGenerationServiceStatsNative,
            _GetGenerationServiceStatsDart
          >('naki_analysis_get_generation_service_stats');
    } catch (_) {
      submitVac2OverlayChunk = null;
      pollGenerationJobs = null;
      getGenerationServiceStats = null;
    }
    open = library.lookupFunction<_OpenNative, _OpenDart>('naki_analysis_open');
    close = library.lookupFunction<_CloseNative, _CloseDart>(
      'naki_analysis_close',
    );
    handleGetSummary = library
        .lookupFunction<_HandleGetSummaryNative, _HandleGetSummaryDart>(
          'naki_analysis_handle_get_summary',
        );
    handleFrameIndexForTimestamp = library
        .lookupFunction<
          _HandleFrameIndexForTimestampNative,
          _HandleFrameIndexForTimestampDart
        >('naki_analysis_handle_frame_index_for_timestamp');
    handleFrameIndexForSourcePacket = library
        .lookupFunction<
          _HandleFrameIndexForSourcePacketNative,
          _HandleFrameIndexForSourcePacketDart
        >('naki_analysis_handle_frame_index_for_source_packet');
    handleGetFramesRange = library
        .lookupFunction<_HandleGetFramesRangeNative, _HandleGetFramesRangeDart>(
          'naki_analysis_handle_get_frames_range',
        );
    handleGetNalusRange = library
        .lookupFunction<_HandleGetNalusRangeNative, _HandleGetNalusRangeDart>(
          'naki_analysis_handle_get_nalus_range',
        );
    handleFrameToNalu = library
        .lookupFunction<_HandleIndexMapNative, _HandleIndexMapDart>(
          'naki_analysis_handle_frame_to_nalu',
        );
    handleNaluToFrame = library
        .lookupFunction<_HandleIndexMapNative, _HandleIndexMapDart>(
          'naki_analysis_handle_nalu_to_frame',
        );
    handleGetFrameBuckets = library
        .lookupFunction<
          _HandleGetFrameBucketsNative,
          _HandleGetFrameBucketsDart
        >('naki_analysis_handle_get_frame_buckets');
    try {
      sizeofQualitySummary = library.lookupFunction<_AbiIntNative, _AbiIntDart>(
        'naki_analysis_sizeof_quality_summary',
      );
      sizeofQualitySample = library.lookupFunction<_AbiIntNative, _AbiIntDart>(
        'naki_analysis_sizeof_quality_sample',
      );
      qualityAnalyze = library
          .lookupFunction<_QualityAnalyzeNative, _QualityAnalyzeDart>(
            'naki_analysis_quality_analyze',
          );
      qualityClose = library
          .lookupFunction<_QualityCloseNative, _QualityCloseDart>(
            'naki_analysis_quality_close',
          );
      qualityGetSummary = library
          .lookupFunction<_QualityGetSummaryNative, _QualityGetSummaryDart>(
            'naki_analysis_quality_get_summary',
          );
      qualityGetSamples = library
          .lookupFunction<_QualityGetSamplesNative, _QualityGetSamplesDart>(
            'naki_analysis_quality_get_samples',
          );
    } catch (_) {
      sizeofQualitySummary = null;
      sizeofQualitySample = null;
      qualityAnalyze = null;
      qualityClose = null;
      qualityGetSummary = null;
      qualityGetSamples = null;
    }

    _validateAbi();
  }

  static _AnalysisNativeBindings? _instance;
  static AnalysisFfiUnavailable? _unavailable;

  static _AnalysisNativeBindings get instance {
    final existing = _instance;
    if (existing != null) return existing;
    final previousFailure = _unavailable;
    if (previousFailure != null) throw previousFailure;
    try {
      return _instance = _loadNativeBindings();
    } catch (e) {
      final unavailable = AnalysisFfiUnavailable(
        'Native analysis symbols are unavailable or incompatible.',
        e,
      );
      _unavailable = unavailable;
      throw unavailable;
    }
  }

  static _AnalysisNativeBindings _loadNativeBindings() {
    try {
      return _AnalysisNativeBindings._(DynamicLibrary.executable());
    } catch (executableError) {
      if (!Platform.isMacOS) rethrow;
      final debugDylib = '${Platform.resolvedExecutable}.debug.dylib';
      if (!File(debugDylib).existsSync()) rethrow;
      try {
        return _AnalysisNativeBindings._(DynamicLibrary.open(debugDylib));
      } catch (debugDylibError) {
        throw AnalysisFfiUnavailable(
          'Failed to load macOS analysis symbols from the executable or debug dylib.',
          '$executableError; $debugDylibError',
        );
      }
    }
  }

  static bool get isAvailable {
    try {
      instance;
      return true;
    } catch (_) {
      return false;
    }
  }

  static AnalysisFfiUnavailable? get unavailableReason {
    try {
      instance;
      return null;
    } on AnalysisFfiUnavailable catch (e) {
      return e;
    }
  }

  late final _AbiIntDart abiVersion;
  late final _AbiIntDart sizeofSummary;
  late final _AbiIntDart sizeofFrameInfo;
  late final _AbiIntDart sizeofNaluInfo;
  late final _AbiIntDart sizeofFrameBucket;
  late final _AbiIntDart sizeofOverlayState;
  late final _LastErrorDart lastError;
  late final _AbiIntDart? sizeofQualitySummary;
  late final _AbiIntDart? sizeofQualitySample;

  late final _SetOverlayDart setOverlay;
  late final _SetOverlayTrackDart setOverlayTrack;
  late final _ClearOverlayTracksDart clearOverlayTracks;
  late final _GenerateVac2BaseDart generateVac2Base;
  late final _GenerateVac2OverlayChunkDart generateVac2OverlayChunk;
  late final _SubmitVac2OverlayChunkDart? submitVac2OverlayChunk;
  late final _PollGenerationJobsDart? pollGenerationJobs;
  late final _GetGenerationServiceStatsDart? getGenerationServiceStats;
  late final _OpenDart open;
  late final _CloseDart close;
  late final _HandleGetSummaryDart handleGetSummary;
  late final _HandleFrameIndexForTimestampDart handleFrameIndexForTimestamp;
  late final _HandleFrameIndexForSourcePacketDart
  handleFrameIndexForSourcePacket;
  late final _HandleGetFramesRangeDart handleGetFramesRange;
  late final _HandleGetNalusRangeDart handleGetNalusRange;
  late final _HandleIndexMapDart handleFrameToNalu;
  late final _HandleIndexMapDart handleNaluToFrame;
  late final _HandleGetFrameBucketsDart handleGetFrameBuckets;
  late final _QualityAnalyzeDart? qualityAnalyze;
  late final _QualityCloseDart? qualityClose;
  late final _QualityGetSummaryDart? qualityGetSummary;
  late final _QualityGetSamplesDart? qualityGetSamples;

  bool get hasGenerationService =>
      submitVac2OverlayChunk != null &&
      pollGenerationJobs != null &&
      getGenerationServiceStats != null;

  bool get hasQualityAnalysis =>
      sizeofQualitySummary != null &&
      sizeofQualitySample != null &&
      qualityAnalyze != null &&
      qualityClose != null &&
      qualityGetSummary != null &&
      qualityGetSamples != null;

  void _validateAbi() {
    final version = abiVersion();
    if (version != _expectedAnalysisAbiVersion) {
      throw StateError(
        'analysis ABI version $version does not match '
        '$_expectedAnalysisAbiVersion',
      );
    }
    _validateSize(
      'NakiAnalysisSummary',
      sizeofSummary(),
      sizeOf<NakiAnalysisSummary>(),
    );
    _validateSize('NakiFrameInfo', sizeofFrameInfo(), sizeOf<NakiFrameInfo>());
    _validateSize('NakiNaluInfo', sizeofNaluInfo(), sizeOf<NakiNaluInfo>());
    _validateSize(
      'NakiFrameBucket',
      sizeofFrameBucket(),
      sizeOf<NakiFrameBucket>(),
    );
    _validateSize(
      'NakiOverlayState',
      sizeofOverlayState(),
      sizeOf<NakiOverlayState>(),
    );
    if (hasQualityAnalysis) {
      _validateSize(
        'NakiQualitySummary',
        sizeofQualitySummary!(),
        sizeOf<NakiQualitySummary>(),
      );
      _validateSize(
        'NakiQualitySample',
        sizeofQualitySample!(),
        sizeOf<NakiQualitySample>(),
      );
    }
  }

  void _validateSize(String name, int nativeSize, int dartSize) {
    if (nativeSize != dartSize) {
      throw StateError(
        '$name size mismatch: native=$nativeSize dart=$dartSize',
      );
    }
  }
}

_AnalysisNativeBindings get _native => _AnalysisNativeBindings.instance;

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

String _fixedInt8String(Array<Int8> bytes, int maxLength) {
  final units = <int>[];
  for (var i = 0; i < maxLength; i++) {
    final value = bytes[i];
    if (value == 0) break;
    units.add(value & 0xff);
  }
  return String.fromCharCodes(units);
}

class AnalysisGenerationJobResult {
  final int jobId;
  final int kind;
  final bool ok;
  final int status;
  final int startFrame;
  final int endFrame;
  final int priority;
  final String hash;
  final String message;

  const AnalysisGenerationJobResult({
    required this.jobId,
    required this.kind,
    required this.ok,
    required this.status,
    required this.startFrame,
    required this.endFrame,
    required this.priority,
    required this.hash,
    required this.message,
  });

  factory AnalysisGenerationJobResult.fromNative(
    NakiAnalysisGenerationJobResult result,
  ) {
    return AnalysisGenerationJobResult(
      jobId: result.jobId,
      kind: result.kind,
      ok: result.ok != 0,
      status: result.status,
      startFrame: result.startFrame,
      endFrame: result.endFrame,
      priority: result.priority,
      hash: _fixedInt8String(result.hash, 65),
      message: _fixedInt8String(result.message, 256),
    );
  }
}

class AnalysisGenerationServiceStats {
  final int workerCount;
  final int activeWorkers;
  final int pendingJobs;
  final int runningJobs;
  final int completedJobs;
  final int failedJobs;
  final int dedupedJobs;
  final int backpressureDropCount;
  final int submittedJobs;

  const AnalysisGenerationServiceStats({
    required this.workerCount,
    required this.activeWorkers,
    required this.pendingJobs,
    required this.runningJobs,
    required this.completedJobs,
    required this.failedJobs,
    required this.dedupedJobs,
    required this.backpressureDropCount,
    required this.submittedJobs,
  });

  factory AnalysisGenerationServiceStats.fromNative(
    NakiAnalysisGenerationServiceStats stats,
  ) {
    return AnalysisGenerationServiceStats(
      workerCount: stats.workerCount,
      activeWorkers: stats.activeWorkers,
      pendingJobs: stats.pendingJobs,
      runningJobs: stats.runningJobs,
      completedJobs: stats.completedJobs,
      failedJobs: stats.failedJobs,
      dedupedJobs: stats.dedupedJobs,
      backpressureDropCount: stats.backpressureDropCount,
      submittedJobs: stats.submittedJobs,
    );
  }
}

enum AnalysisQualityMetric { blockiness, banding, blur, noise, flicker }

class AnalysisQualityDistribution {
  final int count;
  final double mean;
  final double p95;
  final double maximum;

  const AnalysisQualityDistribution({
    required this.count,
    required this.mean,
    required this.p95,
    required this.maximum,
  });

  factory AnalysisQualityDistribution.fromNative(
    NakiQualityDistribution distribution,
  ) {
    return AnalysisQualityDistribution(
      count: distribution.count,
      mean: distribution.mean,
      p95: distribution.p95,
      maximum: distribution.maximum,
    );
  }
}

class AnalysisQualitySample {
  final int sampleIndex;
  final int decodedFrameIndex;
  final int ptsUs;
  final double blockiness;
  final double banding;
  final double blur;
  final double noise;
  final double? flicker;
  final double? averageQp;

  const AnalysisQualitySample({
    required this.sampleIndex,
    required this.decodedFrameIndex,
    required this.ptsUs,
    required this.blockiness,
    required this.banding,
    required this.blur,
    required this.noise,
    required this.flicker,
    required this.averageQp,
  });

  double? valueFor(AnalysisQualityMetric metric) {
    return switch (metric) {
      AnalysisQualityMetric.blockiness => blockiness,
      AnalysisQualityMetric.banding => banding,
      AnalysisQualityMetric.blur => blur,
      AnalysisQualityMetric.noise => noise,
      AnalysisQualityMetric.flicker => flicker,
    };
  }

  factory AnalysisQualitySample.fromNative(NakiQualitySample sample) {
    return AnalysisQualitySample(
      sampleIndex: sample.sampleIndex,
      decodedFrameIndex: sample.decodedFrameIndex,
      ptsUs: sample.ptsUs,
      blockiness: sample.blockiness,
      banding: sample.banding,
      blur: sample.blur,
      noise: sample.noise,
      flicker: sample.flicker < 0 ? null : sample.flicker,
      averageQp: sample.averageQp < 0 ? null : sample.averageQp,
    );
  }
}

class AnalysisQualityReport {
  final int schemaVersion;
  final int videoWidth;
  final int videoHeight;
  final int bitDepth;
  final int sampleIntervalUs;
  final int maxSamples;
  final bool truncated;
  final int unsupportedPixelFrames;
  final Map<AnalysisQualityMetric, AnalysisQualityDistribution> distributions;
  final List<AnalysisQualitySample> samples;

  const AnalysisQualityReport({
    required this.schemaVersion,
    required this.videoWidth,
    required this.videoHeight,
    required this.bitDepth,
    required this.sampleIntervalUs,
    required this.maxSamples,
    required this.truncated,
    required this.unsupportedPixelFrames,
    required this.distributions,
    required this.samples,
  });
}

class AnalysisQualityNative {
  const AnalysisQualityNative._();

  static bool get isAvailable {
    try {
      return _native.hasQualityAnalysis;
    } catch (_) {
      return false;
    }
  }

  static AnalysisQualityReport analyzeSync(
    String videoPath, {
    int sampleIntervalUs = 1000000,
    int maxSamples = 0,
  }) {
    if (videoPath.trim().isEmpty || sampleIntervalUs <= 0 || maxSamples < 0) {
      throw ArgumentError(
        'videoPath, sampleIntervalUs, and maxSamples must be valid',
      );
    }
    final bindings = _native;
    final analyze = bindings.qualityAnalyze;
    final close = bindings.qualityClose;
    final getSummary = bindings.qualityGetSummary;
    final getSamples = bindings.qualityGetSamples;
    if (analyze == null ||
        close == null ||
        getSummary == null ||
        getSamples == null) {
      throw const AnalysisFfiUnavailable(
        'Native quality analysis symbols are unavailable.',
      );
    }

    final path = videoPath.toNativeUtf8(allocator: calloc);
    Pointer<Void> handle = nullptr;
    try {
      handle = analyze(path, sampleIntervalUs, maxSamples);
      if (handle == nullptr) {
        throw StateError(
          _analysisLastError(bindings, 'Quality analysis failed'),
        );
      }
      final summary = calloc<NakiQualitySummary>();
      try {
        if (getSummary(handle, summary) == 0) {
          throw StateError(
            _analysisLastError(bindings, 'Quality summary is unavailable'),
          );
        }
        final native = summary.ref;
        final samples = <AnalysisQualitySample>[];
        const chunkSize = 256;
        final chunk = calloc<NakiQualitySample>(chunkSize);
        try {
          var offset = 0;
          while (offset < native.sampleCount) {
            final requested = (native.sampleCount - offset)
                .clamp(0, chunkSize)
                .toInt();
            final count = getSamples(handle, offset, chunk, requested);
            if (count <= 0) {
              throw StateError(
                _analysisLastError(
                  bindings,
                  'Quality timeline ended before the reported sample count',
                ),
              );
            }
            for (var index = 0; index < count; index++) {
              samples.add(AnalysisQualitySample.fromNative(chunk[index]));
            }
            offset += count;
          }
        } finally {
          calloc.free(chunk);
        }
        return AnalysisQualityReport(
          schemaVersion: native.schemaVersion,
          videoWidth: native.videoWidth,
          videoHeight: native.videoHeight,
          bitDepth: native.bitDepth,
          sampleIntervalUs: native.sampleIntervalUs,
          maxSamples: native.maxSamples,
          truncated: native.truncated != 0,
          unsupportedPixelFrames: native.unsupportedPixelFrames,
          distributions: Map.unmodifiable({
            AnalysisQualityMetric.blockiness:
                AnalysisQualityDistribution.fromNative(native.blockiness),
            AnalysisQualityMetric.banding:
                AnalysisQualityDistribution.fromNative(native.banding),
            AnalysisQualityMetric.blur: AnalysisQualityDistribution.fromNative(
              native.blur,
            ),
            AnalysisQualityMetric.noise: AnalysisQualityDistribution.fromNative(
              native.noise,
            ),
            AnalysisQualityMetric.flicker:
                AnalysisQualityDistribution.fromNative(native.flicker),
          }),
          samples: List.unmodifiable(samples),
        );
      } finally {
        calloc.free(summary);
      }
    } finally {
      calloc.free(path);
      if (handle != nullptr) close(handle);
    }
  }
}

String _analysisLastError(_AnalysisNativeBindings bindings, String fallback) {
  final buffer = calloc<Int8>(512);
  try {
    bindings.lastError(buffer, 512);
    final message = buffer.cast<Utf8>().toDartString().trim();
    return message.isEmpty ? fallback : message;
  } finally {
    calloc.free(buffer);
  }
}

class AnalysisSession {
  Pointer<Void> _handle;
  FileLockHandle? _useLock;

  AnalysisSession._(this._handle, this._useLock);

  static AnalysisSession? open(String analysisPath) {
    final hash = AnalysisCache.hashForAnalysisPath(analysisPath);
    final useLock = AnalysisCache.acquireHashSharedLockSync(hash);
    final analysis = analysisPath.toNativeUtf8(allocator: calloc);
    try {
      final handle = _native.open(analysis);
      if (handle == nullptr) {
        useLock.releaseSync();
        return null;
      }
      return AnalysisSession._(handle, useLock);
    } catch (_) {
      useLock.releaseSync();
      rethrow;
    } finally {
      calloc.free(analysis);
    }
  }

  bool get isOpen => _handle != nullptr;

  void close() {
    if (_handle == nullptr) {
      _useLock?.releaseSync();
      _useLock = null;
      return;
    }
    try {
      _native.close(_handle);
    } finally {
      _handle = nullptr;
      _useLock?.releaseSync();
      _useLock = null;
    }
  }

  AnalysisSummary get summary {
    if (_handle == nullptr) return _emptySummary;
    final ptr = _native.handleGetSummary(_handle);
    if (ptr == nullptr) return _emptySummary;
    return AnalysisSummary.fromNative(ptr.ref);
  }

  int frameIndexForTimestamp({required int ptsUs, required int dtsUs}) {
    if (_handle == nullptr || ptsUs < 0) return -1;
    return _native.handleFrameIndexForTimestamp(_handle, ptsUs, dtsUs);
  }

  int frameIndexForSourcePacket({
    required int packetPos,
    required int packetSize,
    required int packetIndex,
    required int packetPts,
    required int packetDts,
  }) {
    if (_handle == nullptr) return -1;
    return _native.handleFrameIndexForSourcePacket(
      _handle,
      packetPos,
      packetSize,
      packetIndex,
      packetPts,
      packetDts,
    );
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
      final actual = _native
          .handleGetFramesRange(_handle, start, ptr, safeCount)
          .clamp(0, safeCount)
          .toInt();
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
      final actual = _native
          .handleGetNalusRange(_handle, start, ptr, safeCount)
          .clamp(0, safeCount)
          .toInt();
      return List.generate(actual, (i) => _naluInfoAt(ptr, i));
    } finally {
      calloc.free(ptr);
    }
  }

  int frameToNalu(int frameIndex) {
    if (_handle == nullptr || frameIndex < 0) return -1;
    return _native.handleFrameToNalu(_handle, frameIndex);
  }

  int naluToFrame(int naluIndex) {
    if (_handle == nullptr || naluIndex < 0) return -1;
    return _native.handleNaluToFrame(_handle, naluIndex);
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
      final actual = _native
          .handleGetFrameBuckets(_handle, start, bucketSize, ptr, maxCount)
          .clamp(0, maxCount)
          .toInt();
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
  static bool get isAvailable => _AnalysisNativeBindings.isAvailable;

  static AnalysisFfiUnavailable? get unavailableReason =>
      _AnalysisNativeBindings.unavailableReason;

  static bool get hasGenerationService => _native.hasGenerationService;

  /// Set overlay visibility flags.
  static void setOverlay({
    required bool showCuGrid,
    required bool showPredMode,
    required bool showQpHeatmap,
    bool showPredLines = false,
    bool showCuBitCostHeatmap = false,
    double opacity = 0.55,
    int mode = 0,
    int trackFileId = -1,
  }) {
    final state = calloc<NakiOverlayState>();
    try {
      state.ref.showCuGrid = showCuGrid ? 1 : 0;
      state.ref.showPredMode = showPredMode ? 1 : 0;
      state.ref.showQpHeatmap = showQpHeatmap ? 1 : 0;
      state.ref.showPredLines = showPredLines ? 1 : 0;
      state.ref.showCuBitCostHeatmap = showCuBitCostHeatmap ? 1 : 0;
      state.ref.opacityPermille = (opacity.clamp(0.0, 1.0) * 1000).round();
      state.ref.mode = mode;
      state.ref.trackFileId = trackFileId;
      _native.setOverlay(state);
    } finally {
      calloc.free(state);
    }
  }

  static bool setOverlayTrack({
    required int trackFileId,
    required String analysisPath,
  }) {
    final path = analysisPath.toNativeUtf8(allocator: calloc);
    try {
      return _native.setOverlayTrack(trackFileId, path) != 0;
    } finally {
      calloc.free(path);
    }
  }

  static void clearOverlayTracks() {
    _native.clearOverlayTracks();
  }

  /// Generate the progressive VAC2 base cache for a video.
  /// Writes to the per-hash VAC2 base cache.
  static bool generateVac2Base(
    String videoPath,
    String hash,
    int maxCacheBytes,
  ) {
    final video = videoPath.toNativeUtf8(allocator: calloc);
    final hashStr = hash.toNativeUtf8(allocator: calloc);
    final cacheRoot = AppPaths.current.analysisCacheDir.toNativeUtf8(
      allocator: calloc,
    );
    try {
      return _native.generateVac2Base(
            video,
            hashStr,
            cacheRoot,
            maxCacheBytes,
          ) !=
          0;
    } finally {
      calloc.free(video);
      calloc.free(hashStr);
      calloc.free(cacheRoot);
    }
  }

  /// Generate an overlay VACHUNK for an inclusive frame range.
  static bool generateVac2OverlayChunk({
    required String videoPath,
    required String hash,
    required int startFrame,
    required int endFrame,
    required int maxCacheBytes,
  }) {
    final video = videoPath.toNativeUtf8(allocator: calloc);
    final hashStr = hash.toNativeUtf8(allocator: calloc);
    final cacheRoot = AppPaths.current.analysisCacheDir.toNativeUtf8(
      allocator: calloc,
    );
    try {
      return _native.generateVac2OverlayChunk(
            video,
            hashStr,
            cacheRoot,
            startFrame,
            endFrame,
            maxCacheBytes,
          ) !=
          0;
    } finally {
      calloc.free(video);
      calloc.free(hashStr);
      calloc.free(cacheRoot);
    }
  }

  static int submitVac2OverlayChunk({
    required String videoPath,
    required String hash,
    required int startFrame,
    required int endFrame,
    required int maxCacheBytes,
    int priority = 0,
  }) {
    final submit = _native.submitVac2OverlayChunk;
    if (submit == null) return 0;
    final video = videoPath.toNativeUtf8(allocator: calloc);
    final hashStr = hash.toNativeUtf8(allocator: calloc);
    final cacheRoot = AppPaths.current.analysisCacheDir.toNativeUtf8(
      allocator: calloc,
    );
    try {
      return submit(
        video,
        hashStr,
        cacheRoot,
        startFrame,
        endFrame,
        maxCacheBytes,
        priority,
      );
    } finally {
      calloc.free(video);
      calloc.free(hashStr);
      calloc.free(cacheRoot);
    }
  }

  static List<AnalysisGenerationJobResult> pollGenerationJobs({
    int maxCount = 64,
  }) {
    final poll = _native.pollGenerationJobs;
    if (poll == null || maxCount <= 0) return const [];
    final ptr = calloc<NakiAnalysisGenerationJobResult>(maxCount);
    try {
      final count = poll(ptr, maxCount).clamp(0, maxCount).toInt();
      return List.generate(
        count,
        (i) => AnalysisGenerationJobResult.fromNative(ptr[i]),
      );
    } finally {
      calloc.free(ptr);
    }
  }

  static AnalysisGenerationServiceStats? generationServiceStats() {
    final getStats = _native.getGenerationServiceStats;
    if (getStats == null) return null;
    final ptr = calloc<NakiAnalysisGenerationServiceStats>();
    try {
      getStats(ptr);
      return AnalysisGenerationServiceStats.fromNative(ptr.ref);
    } finally {
      calloc.free(ptr);
    }
  }
}

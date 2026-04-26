import 'package:flutter/material.dart';

enum AnalysisCodec {
  unknown(0),
  h264(1),
  hevc(2),
  vvc(3),
  av1(4),
  vp9(5),
  mpeg2(6);

  final int value;
  const AnalysisCodec(this.value);
}

AnalysisCodec analysisCodecFromValue(int value) {
  for (final codec in AnalysisCodec.values) {
    if (codec.value == value) return codec;
  }
  return AnalysisCodec.unknown;
}

String analysisCodecName(AnalysisCodec codec) => switch (codec) {
  AnalysisCodec.h264 => 'H.264',
  AnalysisCodec.hevc => 'H.265/HEVC',
  AnalysisCodec.vvc => 'H.266/VVC',
  AnalysisCodec.av1 => 'AV1',
  AnalysisCodec.vp9 => 'VP9',
  AnalysisCodec.mpeg2 => 'MPEG-2',
  AnalysisCodec.unknown => 'Unknown',
};

String bitstreamUnitTypeName(AnalysisCodec codec, int type) => switch (codec) {
  AnalysisCodec.h264 => _h264NaluTypeName(type),
  AnalysisCodec.hevc => _hevcNaluTypeName(type),
  AnalysisCodec.vvc => _vvcNaluTypeName(type),
  AnalysisCodec.av1 => _av1ObuTypeName(type),
  AnalysisCodec.vp9 => 'VP9_PACKET',
  AnalysisCodec.mpeg2 => _mpeg2StartCodeName(type),
  AnalysisCodec.unknown => 'TYPE_$type',
};

String _h264NaluTypeName(int type) => switch (type) {
  0 => 'UNSPEC_0',
  1 => 'CODED_SLICE_NON_IDR',
  2 => 'CODED_SLICE_DPA',
  3 => 'CODED_SLICE_DPB',
  4 => 'CODED_SLICE_DPC',
  5 => 'CODED_SLICE_IDR',
  6 => 'SEI',
  7 => 'SPS',
  8 => 'PPS',
  9 => 'AUD',
  10 => 'END_OF_SEQUENCE',
  11 => 'END_OF_STREAM',
  12 => 'FILLER_DATA',
  13 => 'SPS_EXT',
  14 => 'PREFIX_NAL',
  15 => 'SUBSET_SPS',
  19 => 'AUXILIARY_SLICE',
  20 => 'EXTENSION_SLICE',
  21 => 'DEPTH_EXTENSION_SLICE',
  _ => 'UNKNOWN ($type)',
};

String _hevcNaluTypeName(int type) => switch (type) {
  0 => 'TRAIL_N',
  1 => 'TRAIL_R',
  2 => 'TSA_N',
  3 => 'TSA_R',
  4 => 'STSA_N',
  5 => 'STSA_R',
  6 => 'RADL_N',
  7 => 'RADL_R',
  8 => 'RASL_N',
  9 => 'RASL_R',
  16 => 'BLA_W_LP',
  17 => 'BLA_W_RADL',
  18 => 'BLA_N_LP',
  19 => 'IDR_W_RADL',
  20 => 'IDR_N_LP',
  21 => 'CRA_NUT',
  32 => 'VPS_NUT',
  33 => 'SPS_NUT',
  34 => 'PPS_NUT',
  35 => 'AUD_NUT',
  36 => 'EOS_NUT',
  37 => 'EOB_NUT',
  38 => 'FD_NUT',
  39 => 'PREFIX_SEI_NUT',
  40 => 'SUFFIX_SEI_NUT',
  _ when type >= 10 && type <= 15 => 'RSV_VCL_$type',
  _ when type >= 22 && type <= 31 => 'RSV_IRAP_VCL_$type',
  _ when type >= 41 && type <= 47 => 'RSV_NVCL_$type',
  _ when type >= 48 && type <= 63 => 'UNSPEC_$type',
  _ => 'UNKNOWN ($type)',
};

String _vvcNaluTypeName(int type) => switch (type) {
  0 => 'TRAIL_NUT',
  1 => 'STSA_NUT',
  2 => 'RADL_NUT',
  3 => 'RASL_NUT',
  4 => 'RSV_VCL_4',
  5 => 'RSV_VCL_5',
  6 => 'RSV_VCL_6',
  7 => 'IDR_N_RADL',
  8 => 'IDR_N_LP',
  9 => 'CRA_NUT',
  10 => 'GDR_NUT',
  11 => 'RSV_IRAP_11',
  12 => 'OPI_NUT',
  13 => 'DCI_NUT',
  14 => 'VPS_NUT',
  15 => 'SPS_NUT',
  16 => 'PPS_NUT',
  17 => 'PREFIX_APS_NUT',
  18 => 'SUFFIX_APS_NUT',
  19 => 'PH_NUT',
  20 => 'AUD_NUT',
  21 => 'EOS_NUT',
  22 => 'EOB_NUT',
  23 => 'PREFIX_SEI_NUT',
  24 => 'SUFFIX_SEI_NUT',
  25 => 'FD_NUT',
  26 => 'RSV_NVCL_26',
  27 => 'RSV_NVCL_27',
  28 => 'UNSPEC_28',
  29 => 'UNSPEC_29',
  30 => 'UNSPEC_30',
  31 => 'UNSPEC_31',
  _ => 'UNKNOWN ($type)',
};

String _av1ObuTypeName(int type) => switch (type) {
  0 => 'OBU_RESERVED_0',
  1 => 'OBU_SEQUENCE_HEADER',
  2 => 'OBU_TEMPORAL_DELIMITER',
  3 => 'OBU_FRAME_HEADER',
  4 => 'OBU_TILE_GROUP',
  5 => 'OBU_METADATA',
  6 => 'OBU_FRAME',
  7 => 'OBU_REDUNDANT_FRAME_HEADER',
  8 => 'OBU_TILE_LIST',
  15 => 'OBU_PADDING',
  _ => 'OBU_RESERVED_$type',
};

String _mpeg2StartCodeName(int type) => switch (type) {
  0x00 => 'PICTURE_START',
  0xB2 => 'USER_DATA',
  0xB3 => 'SEQUENCE_HEADER',
  0xB5 => 'EXTENSION',
  0xB7 => 'SEQUENCE_END',
  0xB8 => 'GROUP_START',
  _ when type >= 0x01 && type <= 0xAF => 'SLICE_$type',
  _ => 'START_CODE_0x${type.toRadixString(16).toUpperCase().padLeft(2, '0')}',
};

Color bitstreamUnitDecorColor(AnalysisCodec codec, int type, {int flags = 0}) {
  if ((flags & 0x04) != 0) return const Color(0xFFFF5252);
  if ((flags & 0x01) != 0) {
    return switch (codec) {
      AnalysisCodec.hevc when type >= 6 && type <= 9 => const Color(0xFF42A5F5),
      AnalysisCodec.vvc when type == 2 || type == 3 => const Color(0xFF42A5F5),
      _ => const Color(0xFF66BB6A),
    };
  }

  return switch (codec) {
    AnalysisCodec.h264 when type == 7 || type == 8 => const Color(0xFF9E9E9E),
    AnalysisCodec.h264 when type == 6 => const Color(0xFFAB47BC),
    AnalysisCodec.hevc when type >= 32 && type <= 34 => const Color(0xFF9E9E9E),
    AnalysisCodec.hevc when type == 39 || type == 40 => const Color(0xFFAB47BC),
    AnalysisCodec.vvc when type >= 14 && type <= 18 => const Color(0xFF9E9E9E),
    AnalysisCodec.vvc when type == 23 || type == 24 => const Color(0xFFAB47BC),
    AnalysisCodec.av1 when type == 1 => const Color(0xFF9E9E9E),
    AnalysisCodec.mpeg2 when type == 0xB3 || type == 0xB5 => const Color(
      0xFF9E9E9E,
    ),
    _ => const Color(0xFF757575),
  };
}

@Deprecated('Use bitstreamUnitTypeName with a codec.')
String h266NaluTypeName(int type) => _vvcNaluTypeName(type);

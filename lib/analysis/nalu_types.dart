/// H.266 / VVC NAL unit type name lookup.
/// Based on ITU-T H.266 Table 7-7.
String h266NaluTypeName(int type) => switch (type) {
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

/// Returns true if the NAL type is a VCL (coded slice) type.
bool isH266VclType(int type) => type >= 0 && type <= 15;

/// Returns true if the NAL type is an IRAP type.
bool isH266IrapType(int type) =>
    type >= 7 && type <= 11; // IDR, CRA, GDR, RSV_IRAP

/// Color for NALU visualization by type.
int naluTypeColor(int type) => switch (type) {
      // VCL slices
      7 || 8 => 0xFFE53935, // IDR — red
      9 => 0xFFFF9800, // CRA — orange
      10 => 0xFFFF5722, // GDR — deep orange
      0 => 0xFF42A5F5, // TRAIL — blue
      1 => 0xFF42A5F5, // STSA — blue
      2 => 0xFF66BB6A, // RADL — green
      3 => 0xFF66BB6A, // RASL — green
      // Parameter sets
      14 => 0xFF9E9E9E, // VPS — grey
      15 => 0xFF757575, // SPS — dark grey
      16 => 0xFFBDBDBD, // PPS — light grey
      17 || 18 => 0xFFBCAAA4, // APS — brown-grey
      // Other
      19 => 0xFF78909C, // PH — blue-grey
      23 || 24 => 0xFFAB47BC, // SEI — purple
      20 => 0xFF8D6E63, // AUD — brown
      _ => 0xFF616161, // unknown — dark grey
    };

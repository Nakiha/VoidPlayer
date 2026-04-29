class AnalysisIpcTrack {
  final int fileId;
  final int slot;
  final String path;
  final String fileName;
  final String hash;
  final int durationUs;

  const AnalysisIpcTrack({
    required this.fileId,
    required this.slot,
    required this.path,
    required this.fileName,
    required this.hash,
    required this.durationUs,
  });

  Map<String, Object?> toJson() => {
    'fileId': fileId,
    'slot': slot,
    'path': path,
    'fileName': fileName,
    'hash': hash,
    'durationUs': durationUs,
  };

  static AnalysisIpcTrack fromJson(Map<String, Object?> json) {
    return AnalysisIpcTrack(
      fileId: (json['fileId'] as num?)?.toInt() ?? -1,
      slot: (json['slot'] as num?)?.toInt() ?? -1,
      path: json['path'] as String? ?? '',
      fileName: json['fileName'] as String? ?? '',
      hash: json['hash'] as String? ?? '',
      durationUs: (json['durationUs'] as num?)?.toInt() ?? 0,
    );
  }
}

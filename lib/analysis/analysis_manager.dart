import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;

import 'analysis_cache.dart';
import 'analysis_ffi.dart';

enum AnalysisState {
  idle,
  computingHash,
  generating,
  loading,
  loaded,
  error,
}

/// Dart-side state machine for the analysis generation + loading flow.
///
/// The UI (AnalysisPanel) listens to this via [ChangeNotifier] to show
/// progress / error / loaded states.
class AnalysisManager extends ChangeNotifier {
  AnalysisManager._();
  static final AnalysisManager instance = AnalysisManager._();

  AnalysisState _state = AnalysisState.idle;
  String? _errorMessage;
  String? _generatingFileName;
  String? _loadedHash;

  AnalysisState get state => _state;
  String? get errorMessage => _errorMessage;
  String? get generatingFileName => _generatingFileName;
  String? get loadedHash => _loadedHash;
  bool get isLoaded => _state == AnalysisState.loaded;

  /// Full flow: compute hash → check cache → generate if needed → load.
  /// Returns the hash on success, null on failure.
  Future<String?> ensureAndLoad(String videoPath) async {
    final fileName = p.basename(videoPath);

    // Step 1: compute hash
    _setState(AnalysisState.computingHash);
    final String hash;
    try {
      hash = await _computeHash(videoPath);
    } catch (e) {
      _setError('Failed to hash file: $e');
      return null;
    }

    // Step 2: check cache
    if (AnalysisCache.hasEntry(hash)) {
      // Step 5 (cached path): load directly
      return _loadFromCache(hash, fileName, videoPath);
    }

    // Step 3: generate
    _state = AnalysisState.generating;
    _generatingFileName = fileName;
    _errorMessage = null;
    notifyListeners();

    final ok = AnalysisFfi.generateAnalysis(videoPath, hash);
    if (!ok) {
      _setError('Unsupported codec or generation failed for $fileName');
      return null;
    }

    // Step 4: write index
    await AnalysisCache.addEntry(hash, fileName, videoPath);

    // Step 5: load
    return _loadFromCache(hash, fileName, videoPath);
  }

  Future<String?> _loadFromCache(String hash, String name, String path) async {
    _setState(AnalysisState.loading);
    final vbs2 = AnalysisCache.vbs2Path(hash);
    final vbi = AnalysisCache.vbiPath(hash);
    final vbt = AnalysisCache.vbtPath(hash);

    // VBS2 is optional — pass empty string if file doesn't exist
    final vbs2Arg = File(vbs2).existsSync() ? vbs2 : '';
    final ok = AnalysisFfi.load(vbs2Arg, vbi, vbt);
    if (!ok) {
      _setError('Failed to load analysis for $name');
      return null;
    }

    _loadedHash = hash;
    _setState(AnalysisState.loaded);
    return hash;
  }

  void unload() {
    if (_state == AnalysisState.loaded) {
      AnalysisFfi.unload();
    }
    _loadedHash = null;
    _setState(AnalysisState.idle);
  }

  // ---- Internal ----

  void _setState(AnalysisState s) {
    _state = s;
    _errorMessage = null;
    _generatingFileName = null;
    notifyListeners();
  }

  void _setError(String msg) {
    _state = AnalysisState.error;
    _errorMessage = msg;
    _generatingFileName = null;
    notifyListeners();
  }

  /// SHA-256 of the first 1 MB of the file, returned as hex string.
  static Future<String> _computeHash(String path) async {
    const int chunkSize = 1024 * 1024; // 1 MB
    final file = File(path);
    // Read-first-chunk synchronously for simplicity (file is local SSD).
    final Uint8List bytes;
    final raf = await file.open();
    try {
      bytes = await raf.read(chunkSize);
    } finally {
      await raf.close();
    }

    // Simple FNV-1a 128-bit hash (no external dependency needed).
    // For cache purposes this is sufficient — collisions extremely unlikely.
    return _fnv1aHex(bytes);
  }

  /// FNV-1a inspired hex digest. Not cryptographic but collision-resistant
  /// enough for a local file cache.
  static String _fnv1aHex(Uint8List data) {
    // Use two 64-bit FNV-1a passes with different offsets for 128-bit result
    int h1 = 0xcbf29ce484222325;
    int h2 = 0x100000001b3;
    for (final b in data) {
      h1 ^= b;
      h1 = (h1 * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF;
      h2 ^= b;
      h2 = (h2 * 0xcbf29ce484222325) & 0xFFFFFFFFFFFFFFFF;
    }
    return '${h1.toRadixString(16).padLeft(16, '0')}${h2.toRadixString(16).padLeft(16, '0')}';
  }
}

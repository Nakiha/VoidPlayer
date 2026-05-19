typedef AnalysisWindowRequest = ({String hash, String? fileName});

abstract interface class AnalysisProcessHost {
  String? get analysisTestScriptPath;
  set analysisTestScriptPath(String? value);

  bool get silentUiTest;
  set silentUiTest(bool value);

  int? get analysisIpcPort;
  set analysisIpcPort(int? value);

  String? get analysisIpcToken;
  set analysisIpcToken(String? value);

  int get accentColorValue;
  set accentColorValue(int value);

  int get analysisProcessCount;
  Map<String, int> get analysisExitCodes;
  bool get supportsExternalAnalysisWindows;

  Future<void> showAnalysisWindow(
    String hash, {
    String? fileName,
    void Function()? onExit,
  });

  Future<void> showAnalysisWindows(
    List<AnalysisWindowRequest> windows, {
    void Function()? onExit,
  });

  bool activateAnalysisWindows();
  Future<bool> waitForAnalysisProcessCount(int count, Duration timeout);
  Future<void> closeAllAnalysisWindows();
}

class UnsupportedAnalysisProcessHost implements AnalysisProcessHost {
  String? _analysisTestScriptPath;
  bool _silentUiTest = false;
  int? _analysisIpcPort;
  String? _analysisIpcToken;
  int _accentColorValue = 0xFF0078D4;

  @override
  String? get analysisTestScriptPath => _analysisTestScriptPath;

  @override
  set analysisTestScriptPath(String? value) {
    _analysisTestScriptPath = value;
  }

  @override
  bool get silentUiTest => _silentUiTest;

  @override
  set silentUiTest(bool value) {
    _silentUiTest = value;
  }

  @override
  int? get analysisIpcPort => _analysisIpcPort;

  @override
  set analysisIpcPort(int? value) {
    _analysisIpcPort = value;
  }

  @override
  String? get analysisIpcToken => _analysisIpcToken;

  @override
  set analysisIpcToken(String? value) {
    _analysisIpcToken = value;
  }

  @override
  int get accentColorValue => _accentColorValue;

  @override
  set accentColorValue(int value) {
    _accentColorValue = value;
  }

  @override
  int get analysisProcessCount => 0;

  @override
  Map<String, int> get analysisExitCodes => const {};

  @override
  bool get supportsExternalAnalysisWindows => false;

  @override
  Future<void> showAnalysisWindow(
    String hash, {
    String? fileName,
    void Function()? onExit,
  }) async {}

  @override
  Future<void> showAnalysisWindows(
    List<AnalysisWindowRequest> windows, {
    void Function()? onExit,
  }) async {}

  @override
  bool activateAnalysisWindows() => false;

  @override
  Future<bool> waitForAnalysisProcessCount(int count, Duration timeout) async {
    return count == 0;
  }

  @override
  Future<void> closeAllAnalysisWindows() async {
    _analysisIpcPort = null;
    _analysisIpcToken = null;
  }
}

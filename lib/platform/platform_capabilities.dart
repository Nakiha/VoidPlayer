class PlatformCapabilities {
  final bool nativePlayback;
  final bool localFilePlayback;
  final bool nativeFilePicker;
  final bool externalAnalysisWindows;
  final bool nativeViewportCapture;
  final bool pathLauncher;

  const PlatformCapabilities({
    required this.nativePlayback,
    required this.localFilePlayback,
    required this.nativeFilePicker,
    required this.externalAnalysisWindows,
    required this.nativeViewportCapture,
    required this.pathLauncher,
  });

  static const windows = PlatformCapabilities(
    nativePlayback: true,
    localFilePlayback: true,
    nativeFilePicker: true,
    externalAnalysisWindows: true,
    nativeViewportCapture: true,
    pathLauncher: true,
  );

  static const macOSPhase1 = PlatformCapabilities(
    nativePlayback: false,
    localFilePlayback: true,
    nativeFilePicker: true,
    externalAnalysisWindows: false,
    nativeViewportCapture: false,
    pathLauncher: true,
  );
}

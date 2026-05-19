class PlatformCapabilities {
  final bool nativePlayback;
  final bool nativeFilePicker;
  final bool externalAnalysisWindows;
  final bool nativeViewportCapture;
  final bool pathLauncher;

  const PlatformCapabilities({
    required this.nativePlayback,
    required this.nativeFilePicker,
    required this.externalAnalysisWindows,
    required this.nativeViewportCapture,
    required this.pathLauncher,
  });

  static const windows = PlatformCapabilities(
    nativePlayback: true,
    nativeFilePicker: true,
    externalAnalysisWindows: true,
    nativeViewportCapture: true,
    pathLauncher: true,
  );

  static const macOSPhase1 = PlatformCapabilities(
    nativePlayback: false,
    nativeFilePicker: false,
    externalAnalysisWindows: false,
    nativeViewportCapture: false,
    pathLauncher: true,
  );
}

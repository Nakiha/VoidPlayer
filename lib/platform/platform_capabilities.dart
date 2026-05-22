class PlatformCapabilities {
  final bool nativePlayback;
  final bool localFilePlayback;
  final bool networkMediaPlayback;
  final bool sshRemoteMediaPlayback;
  final bool nativeFilePicker;
  final bool externalAnalysisWindows;
  final bool nativeViewportCapture;
  final bool pathLauncher;

  const PlatformCapabilities({
    required this.nativePlayback,
    required this.localFilePlayback,
    required this.networkMediaPlayback,
    required this.sshRemoteMediaPlayback,
    required this.nativeFilePicker,
    required this.externalAnalysisWindows,
    required this.nativeViewportCapture,
    required this.pathLauncher,
  });

  static const windows = PlatformCapabilities(
    nativePlayback: true,
    localFilePlayback: true,
    networkMediaPlayback: true,
    sshRemoteMediaPlayback: true,
    nativeFilePicker: true,
    externalAnalysisWindows: true,
    nativeViewportCapture: true,
    pathLauncher: true,
  );

  static const macOSPhase1 = PlatformCapabilities(
    nativePlayback: true,
    localFilePlayback: true,
    networkMediaPlayback: false,
    sshRemoteMediaPlayback: false,
    nativeFilePicker: true,
    externalAnalysisWindows: false,
    nativeViewportCapture: true,
    pathLauncher: true,
  );
}

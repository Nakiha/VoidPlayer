enum CapabilityState {
  supported,
  unsupported,
  experimental,
  sandboxLimited,
  localOnly,
}

class PlatformCapability {
  final CapabilityState state;
  final String? detail;

  const PlatformCapability(this.state, {this.detail});

  String get statusLabel {
    return switch (state) {
      CapabilityState.supported => 'Supported',
      CapabilityState.unsupported => 'Not available',
      CapabilityState.experimental => 'Experimental',
      CapabilityState.sandboxLimited => 'Sandbox limited',
      CapabilityState.localOnly => 'Local only',
    };
  }

  String get userMessage {
    final text = detail;
    if (text != null && text.isNotEmpty) return text;
    return statusLabel;
  }

  bool get isAvailable {
    switch (state) {
      case CapabilityState.supported:
      case CapabilityState.experimental:
      case CapabilityState.sandboxLimited:
      case CapabilityState.localOnly:
        return true;
      case CapabilityState.unsupported:
        return false;
    }
  }
}

String? firstCapabilityUserMessage(Iterable<PlatformCapability> capabilities) {
  for (final capability in capabilities) {
    if (!capability.isAvailable) return capability.userMessage;
  }
  for (final capability in capabilities) {
    if (capability.state != CapabilityState.supported) {
      return capability.userMessage;
    }
  }
  return null;
}

class PlatformCapabilities {
  final PlatformCapability nativePlaybackCapability;
  final PlatformCapability localFilePlaybackCapability;
  final PlatformCapability networkMediaPlaybackCapability;
  final PlatformCapability sshRemoteMediaPlaybackCapability;
  final PlatformCapability nativeFilePickerCapability;
  final PlatformCapability analysisOverlaysCapability;
  final PlatformCapability nativeViewportCaptureCapability;
  final PlatformCapability pathLauncherCapability;

  const PlatformCapabilities({
    required this.nativePlaybackCapability,
    required this.localFilePlaybackCapability,
    required this.networkMediaPlaybackCapability,
    required this.sshRemoteMediaPlaybackCapability,
    required this.nativeFilePickerCapability,
    required this.analysisOverlaysCapability,
    required this.nativeViewportCaptureCapability,
    required this.pathLauncherCapability,
  });

  bool get nativePlayback => nativePlaybackCapability.isAvailable;
  bool get localFilePlayback => localFilePlaybackCapability.isAvailable;
  bool get networkMediaPlayback => networkMediaPlaybackCapability.isAvailable;
  bool get sshRemoteMediaPlayback =>
      sshRemoteMediaPlaybackCapability.isAvailable;
  bool get nativeFilePicker => nativeFilePickerCapability.isAvailable;
  bool get analysisOverlays => analysisOverlaysCapability.isAvailable;
  bool get nativeViewportCapture => nativeViewportCaptureCapability.isAvailable;
  bool get pathLauncher => pathLauncherCapability.isAvailable;

  static const windows = PlatformCapabilities(
    nativePlaybackCapability: PlatformCapability(CapabilityState.supported),
    localFilePlaybackCapability: PlatformCapability(CapabilityState.supported),
    networkMediaPlaybackCapability: PlatformCapability(
      CapabilityState.supported,
    ),
    sshRemoteMediaPlaybackCapability: PlatformCapability(
      CapabilityState.supported,
    ),
    nativeFilePickerCapability: PlatformCapability(CapabilityState.supported),
    analysisOverlaysCapability: PlatformCapability(CapabilityState.supported),
    nativeViewportCaptureCapability: PlatformCapability(
      CapabilityState.supported,
    ),
    pathLauncherCapability: PlatformCapability(CapabilityState.supported),
  );

  static const macOSPhase1 = PlatformCapabilities(
    nativePlaybackCapability: PlatformCapability(
      CapabilityState.localOnly,
      detail: 'macOS native playback is currently limited to local media.',
    ),
    localFilePlaybackCapability: PlatformCapability(
      CapabilityState.sandboxLimited,
      detail: 'Local files require user-selected sandbox access.',
    ),
    networkMediaPlaybackCapability: PlatformCapability(
      CapabilityState.unsupported,
      detail: 'macOS phase 1 does not enable network media playback.',
    ),
    sshRemoteMediaPlaybackCapability: PlatformCapability(
      CapabilityState.unsupported,
      detail: 'macOS phase 1 does not enable SSH remote media playback.',
    ),
    nativeFilePickerCapability: PlatformCapability(
      CapabilityState.sandboxLimited,
      detail: 'NSOpenPanel grants read-only access for selected files.',
    ),
    analysisOverlaysCapability: PlatformCapability(
      CapabilityState.localOnly,
      detail: 'Overlay rendering is available for local native playback.',
    ),
    nativeViewportCaptureCapability: PlatformCapability(
      CapabilityState.experimental,
      detail: 'Viewport capture reports native presentation diagnostics.',
    ),
    pathLauncherCapability: PlatformCapability(
      CapabilityState.sandboxLimited,
      detail: 'macOS path reveal/open may be denied outside sandbox scope.',
    ),
  );
}

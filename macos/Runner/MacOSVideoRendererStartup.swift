import Foundation

struct MacOSVideoRendererStartup {
  let texture: MacOSFlutterTextureBridge
  let backendName: String
  let nativePlayer: MacOSNativePlayerSession?
  let tracks: [[String: Any]]
  let trackDurationUs: Int
  let initialPresentedPtsUs: Int
  let initialPresentedDtsUs: Int
  let presentationTargetInstalled: Bool
}

enum MacOSVideoRendererStartupFactory {
  static func make(arguments: Any?) throws -> MacOSVideoRendererStartup {
    let paths = MacOSFlutterArguments.stringListArg(arguments, "videoPaths")
    let requestedWidth = max(16, MacOSFlutterArguments.intArg(arguments, "width") ?? 1920)
    let requestedHeight = max(16, MacOSFlutterArguments.intArg(arguments, "height") ?? 1080)
    let firstPath = paths.first ?? "macos-synthetic://color-bars"

    if firstPath.hasPrefix("macos-synthetic://") {
      return syntheticStartup(
        paths: paths,
        requestedWidth: requestedWidth,
        requestedHeight: requestedHeight
      )
    }
    return try nativeStartup(
      paths: paths,
      firstPath: firstPath,
      requestedWidth: requestedWidth,
      requestedHeight: requestedHeight
    )
  }

  private static func syntheticStartup(
    paths: [String],
    requestedWidth: Int,
    requestedHeight: Int
  ) -> MacOSVideoRendererStartup {
    let texture = MacOSFlutterTextureBridge(
      width: requestedWidth,
      height: requestedHeight
    )
    let tracks = paths.enumerated().map { index, path in
      MacOSVideoTrackPayload.syntheticTrack(
        fileId: index,
        slot: index,
        path: path,
        width: requestedWidth,
        height: requestedHeight,
        durationUs: MacOSVideoTrackPayload.syntheticDurationUs
      )
    }
    return MacOSVideoRendererStartup(
      texture: texture,
      backendName: "synthetic-texture",
      nativePlayer: nil,
      tracks: tracks,
      trackDurationUs: MacOSVideoTrackPayload.syntheticDurationUs,
      initialPresentedPtsUs: 0,
      initialPresentedDtsUs: 0,
      presentationTargetInstalled: false
    )
  }

  private static func nativeStartup(
    paths: [String],
    firstPath: String,
    requestedWidth: Int,
    requestedHeight: Int
  ) throws -> MacOSVideoRendererStartup {
    guard let session = MacOSNativePlayerSession() else {
      throw MacOSNativePlayerError.failed("failed to allocate macOS native player")
    }
    try session.open(path: firstPath)
    let texture = MacOSFlutterTextureBridge(
      nativeWidth: requestedWidth,
      nativeHeight: requestedHeight
    )
    let firstFrame = try texture.updateFromNativePlayer(
      session,
      maxTrackSlots: 1,
      waitTimeoutMs: 3_000
    )
    let trackWidth = session.width() > 0 ? session.width() : firstFrame.width
    let trackHeight = session.height() > 0 ? session.height() : firstFrame.height
    let sessionDurationUs = session.durationUs()
    let trackDurationUs = sessionDurationUs > 0 ? sessionDurationUs : MacOSVideoTrackPayload.syntheticDurationUs
    var tracks = [
      MacOSVideoTrackPayload.track(
        fileId: 0,
        slot: 0,
        path: firstPath,
        width: trackWidth,
        height: trackHeight,
        durationUs: trackDurationUs,
        formatName: MacOSVideoTrackPayload.nativeFormatName,
        codecName: MacOSVideoTrackPayload.nativeCodecName,
        codecLongName: MacOSVideoTrackPayload.nativeCodecLongName,
        decoderName: session.decoderName()
      )
    ]
    for path in paths.dropFirst() {
      let fileId = (tracks.map { MacOSFlutterArguments.intValue($0["fileId"]) ?? 0 }.max() ?? -1) + 1
      let metadata = try session.addTrack(path: path, fileId: fileId)
      tracks.append(MacOSVideoTrackPayload.nativeTrack(
        path: path,
        metadata: metadata,
        decoderName: session.decoderName()
      ))
    }
    return MacOSVideoRendererStartup(
      texture: texture,
      backendName: MacOSVideoTrackPayload.nativeFormatName,
      nativePlayer: session,
      tracks: tracks,
      trackDurationUs: trackDurationUs,
      initialPresentedPtsUs: firstFrame.ptsUs,
      initialPresentedDtsUs: MacOSFramePresentationState.normalizedDtsUs(firstFrame),
      presentationTargetInstalled: session.rendererOwnedPresentationActive()
    )
  }
}

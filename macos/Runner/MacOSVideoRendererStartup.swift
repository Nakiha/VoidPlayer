import Foundation

struct MacOSVideoRendererStartup {
  let texture: MacOSVideoTexture
  let nativeTexture: MacOSFlutterTextureBridge?
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
    let useHardwareDecode =
      MacOSFlutterArguments.boolArg(arguments, "useHardwareDecode") ?? true
    let viewportBackgroundColor = MacOSFlutterArguments.uint32Arg(arguments, "color")
    guard let firstPath = paths.first, !firstPath.isEmpty else {
      throw MacOSNativePlayerError.failed("createPlayer requires at least one media path")
    }

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
      requestedHeight: requestedHeight,
      useHardwareDecode: useHardwareDecode,
      viewportBackgroundColor: viewportBackgroundColor
    )
  }

  private static func syntheticStartup(
    paths: [String],
    requestedWidth: Int,
    requestedHeight: Int
  ) -> MacOSVideoRendererStartup {
    let texture = MacOSSyntheticTextureBridge(
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
      nativeTexture: nil,
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
    requestedHeight: Int,
    useHardwareDecode: Bool,
    viewportBackgroundColor: UInt32?
  ) throws -> MacOSVideoRendererStartup {
    guard let session = MacOSNativePlayerSession() else {
      throw MacOSNativePlayerError.failed("failed to allocate macOS native player")
    }
    session.setHardwareDecodeEnabled(useHardwareDecode)
    if let viewportBackgroundColor {
      session.setBackgroundColor(viewportBackgroundColor)
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
    let firstMetadata = try session.trackMetadata(fileId: 0)
    var tracks = [
      MacOSVideoTrackPayload.nativeTrack(
        path: firstPath,
        metadata: MacOSNativeTrackMetadata(
          fileId: firstMetadata.fileId,
          slot: firstMetadata.slot,
          width: firstMetadata.width > 0 ? firstMetadata.width : trackWidth,
          height: firstMetadata.height > 0 ? firstMetadata.height : trackHeight,
          durationUs: firstMetadata.durationUs > 0 ? firstMetadata.durationUs : trackDurationUs,
          startTimeUs: firstMetadata.startTimeUs,
          bitRate: firstMetadata.bitRate,
          formatName: firstMetadata.formatName,
          codecName: firstMetadata.codecName,
          codecLongName: firstMetadata.codecLongName,
          decoderName: firstMetadata.decoderName
        ),
        decoderName: session.decoderName()
      )
    ]
    for path in paths.dropFirst() {
      let fileId = (tracks.map { MacOSFlutterArguments.intValue($0["fileId"]) ?? 0 }.max() ?? -1) + 1
      let metadata = try session.addTrack(
        path: path,
        fileId: fileId,
        useHardwareDecode: useHardwareDecode
      )
      tracks.append(MacOSVideoTrackPayload.nativeTrack(
        path: path,
        metadata: metadata,
        decoderName: session.decoderName()
      ))
    }
    return MacOSVideoRendererStartup(
      texture: texture,
      nativeTexture: texture,
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

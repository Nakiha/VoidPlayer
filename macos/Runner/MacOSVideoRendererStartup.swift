import AppKit
import Foundation
import FlutterMacOS

struct MacOSVideoRendererStartup {
  let texture: MacOSVideoSurface
  let flutterTexture: FlutterTexture?
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
    guard paths.count <= MacOSVideoTrackPayload.maxTrackCount else {
      throw MacOSNativePlayerError.failed(
        "createPlayer supports at most \(MacOSVideoTrackPayload.maxTrackCount) tracks"
      )
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
      flutterTexture: texture,
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
    if let unsupported = paths.compactMap({ path -> (path: String, reason: String)? in
      guard let reason = MacOSMediaInputGuard.unsupportedReason(path: path) else {
        return nil
      }
      return (path, reason)
    }).first {
      throw MacOSNativePlayerError.failed("\(unsupported.reason): \(unsupported.path)")
    }
    let probedTracks = paths.compactMap { path in
      try? MacOSNativePlayerSession.probeTrack(path: path)
    }
    let hasHDRTrack = probedTracks.contains { $0.isHDR }
    let configuration = MacOSPresentationConfiguration.resolve(
      hasHDRTrack: hasHDRTrack,
      screen: NSScreen.main
    )
    MacOSPresentationConfiguration.updateCurrent(configuration)
    NSLog(
      "VoidPlayer macOS presentation policy: request=%@ mode=%@ reason=%@ hdrTrack=%@",
      configuration.request,
      configuration.mode.rawValue,
      configuration.reason,
      hasHDRTrack ? "true" : "false"
    )

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
    guard texture.installNativePresentationTarget(
      session,
      maxTrackSlots: max(1, paths.count),
      refresh: false
    ) else {
      throw MacOSNativePlayerError.failed(
        "failed to install renderer-owned Metal presentation target ring"
      )
    }
    let sessionDurationUs = session.durationUs()
    let trackDurationUs = max(0, sessionDurationUs)
    let firstMetadata = try session.trackMetadata(fileId: 0)
    let firstFrame = session.lastRendererOwnedFrameInfo()
    let trackWidth = firstMetadata.width > 0 ? firstMetadata.width : (firstFrame?.width ?? requestedWidth)
    let trackHeight = firstMetadata.height > 0 ? firstMetadata.height : (firstFrame?.height ?? requestedHeight)
    var tracks = [
      MacOSVideoTrackPayload.nativeTrack(
        path: firstPath,
        metadata: MacOSNativeTrackMetadata(
          fileId: firstMetadata.fileId,
          slot: firstMetadata.slot,
          width: firstMetadata.width > 0 ? firstMetadata.width : trackWidth,
          height: firstMetadata.height > 0 ? firstMetadata.height : trackHeight,
          durationUs: max(0, firstMetadata.durationUs),
          startTimeUs: firstMetadata.startTimeUs,
          bitRate: firstMetadata.bitRate,
          formatName: firstMetadata.formatName,
          codecName: firstMetadata.codecName,
          codecLongName: firstMetadata.codecLongName,
          decoderName: firstMetadata.decoderName,
          colorRange: firstMetadata.colorRange,
          colorMatrix: firstMetadata.colorMatrix,
          colorTransfer: firstMetadata.colorTransfer,
          colorPrimaries: firstMetadata.colorPrimaries
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
      flutterTexture: nil,
      nativeTexture: texture,
      backendName: MacOSVideoTrackPayload.nativeFormatName,
      nativePlayer: session,
      tracks: tracks,
      trackDurationUs: trackDurationUs,
      initialPresentedPtsUs: firstFrame?.ptsUs ?? 0,
      initialPresentedDtsUs: firstFrame.map {
        MacOSFramePresentationState.normalizedDtsUs($0)
      } ?? 0,
      presentationTargetInstalled: session.rendererOwnedPresentationActive()
    )
  }
}

import FlutterMacOS
import Foundation

struct MacOSTransportContext {
  let nativeBackendActive: Bool
  let player: MacOSNativePlayerSession?
  let texture: MacOSFlutterTextureBridge?
  let textureRegistered: Bool
  let playback: MacOSPlaybackController
  let presentationState: MacOSFramePresentationState
  let activeDurationUs: Int
  let maxTrackSlots: Int
  let userData: UnsafeMutableRawPointer
  let markFrameAvailable: () -> Void
  let emitSeekPreviewPresented: (Int?, Int) -> Void
}

final class MacOSTransportController {
  func setTrackOffset(arguments: Any?, player: MacOSNativePlayerSession?) {
    let fileId = MacOSFlutterArguments.intArg(arguments, "fileId") ?? -1
    let offsetUs = MacOSFlutterArguments.intArg(arguments, "offsetUs") ?? 0
    player?.setTrackOffset(fileId: fileId, offsetUs: offsetUs)
  }

  func setLoopRange(arguments: Any?, player: MacOSNativePlayerSession?) {
    player?.setLoopRange(
      enabled: MacOSFlutterArguments.boolArg(arguments, "enabled") ?? false,
      startUs: MacOSFlutterArguments.intArg(arguments, "startUs") ?? 0,
      endUs: MacOSFlutterArguments.intArg(arguments, "endUs") ?? 0
    )
  }

  func setAudibleTrack(arguments: Any?, player: MacOSNativePlayerSession?) {
    let fileId = MacOSFlutterArguments.intArg(arguments, "fileId") ?? -1
    player?.setAudibleTrack(fileId)
  }

  func setSpeed(arguments: Any?, player: MacOSNativePlayerSession?) {
    let speed = max(0.01, MacOSFlutterArguments.doubleArg(arguments, "speed") ?? 1.0)
    player?.setSpeed(speed)
  }

  func currentPts(player: MacOSNativePlayerSession?, presentationState: MacOSFramePresentationState) -> Int {
    presentationState.setCurrentPts(player?.currentPtsUs() ?? presentationState.currentPtsUs)
    return presentationState.currentPtsUs
  }

  func seekAndRefresh(
    targetPtsUs: Int,
    requestId: Int?,
    resumeAfterSeek: Bool,
    context: MacOSTransportContext
  ) -> FlutterError? {
    context.playback.stopForBlockingCommand(player: context.player, pausePlayer: true)
    let settledPtsUs = max(0, min(context.activeDurationUs, targetPtsUs))
    context.presentationState.setCurrentPts(settledPtsUs)
    if let error = refreshDecodedFrameIfNeeded(targetPtsUs: settledPtsUs, context: context) {
      return error
    }
    context.markFrameAvailable()
    context.emitSeekPreviewPresented(requestId, settledPtsUs)
    if resumeAfterSeek {
      context.playback.resumeIfNeeded(
        true,
        player: context.player,
        texture: context.texture,
        textureRegistered: context.textureRegistered,
        maxTrackSlots: context.maxTrackSlots,
        userData: context.userData,
        presentationState: context.presentationState
      )
    }
    return nil
  }

  func stepAndRefresh(
    forward: Bool,
    context: MacOSTransportContext
  ) -> FlutterError? {
    context.playback.stopForBlockingCommand(player: context.player, pausePlayer: false)
    guard context.nativeBackendActive,
          let player = context.player,
          context.texture != nil else {
      return nil
    }
    if let error = MacOSNativeFrameRefresh.stepAndRefresh(
      player: player,
      forward: forward,
      presentationState: context.presentationState,
      framePump: context.playback.framePumpForRefresh
    ) {
      return error
    }
    context.markFrameAvailable()
    return nil
  }

  private func refreshDecodedFrameIfNeeded(
    targetPtsUs: Int,
    context: MacOSTransportContext
  ) -> FlutterError? {
    guard context.nativeBackendActive,
          let player = context.player,
          let texture = context.texture else {
      return nil
    }

    return MacOSNativeFrameRefresh.seekAndRefresh(
      player: player,
      texture: texture,
      targetPtsUs: targetPtsUs,
      maxTrackSlots: context.maxTrackSlots,
      presentationState: context.presentationState,
      framePump: context.playback.framePumpForRefresh
    )
  }
}

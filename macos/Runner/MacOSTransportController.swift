import FlutterMacOS
import Foundation

struct MacOSTransportContext {
  let nativeBackendActive: Bool
  let player: MacOSNativePlayerSession?
  let texture: MacOSNativeTargetRing?
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
  private struct PendingSeekPreview {
    let serial: Int
    let requestId: Int?
    let targetPtsUs: Int
  }

  private var seekRefreshSerial = 0
  private var pendingSeekPreview: PendingSeekPreview?

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
    seekRefreshSerial &+= 1
    let seekSerial = seekRefreshSerial
    pendingSeekPreview = nil
    context.playback.stopForBlockingCommand(player: context.player, pausePlayer: true)
    let settledPtsUs = max(0, min(context.activeDurationUs, targetPtsUs))
    context.presentationState.setCurrentPts(settledPtsUs)
    let refreshResult = refreshDecodedFrameIfNeeded(
      targetPtsUs: settledPtsUs,
      timeoutMs: resumeAfterSeek ? 3_000 : 180,
      context: context
    )
    if case .failed(let error) = refreshResult {
      pendingSeekPreview = nil
      return error
    }
    if case .presented = refreshResult {
      pendingSeekPreview = nil
      context.markFrameAvailable()
      context.emitSeekPreviewPresented(requestId, settledPtsUs)
    }
    if case .pending = refreshResult,
       !resumeAfterSeek {
      schedulePausedSeekLateFrameDelivery(
        targetPtsUs: settledPtsUs,
        requestId: requestId,
        seekSerial: seekSerial,
        context: context
      )
    }
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

  private func schedulePausedSeekLateFrameDelivery(
    targetPtsUs: Int,
    requestId: Int?,
    seekSerial: Int,
    context: MacOSTransportContext
  ) {
    let pumpReady = context.playback.ensurePresentationPump(
      player: context.player,
      texture: context.texture,
      maxTrackSlots: context.maxTrackSlots,
      userData: context.userData,
      presentationState: context.presentationState
    )
    MacOSProfilerLog.log(
      "VoidPlayer macOS paused seek pending; late frame pumpReady=\(pumpReady) targetPtsUs=\(targetPtsUs)"
    )
    NSLog(
      "VoidPlayer macOS paused seek pending: pumpReady=%@ targetPtsUs=%d requestId=%d",
      pumpReady ? "true" : "false",
      targetPtsUs,
      requestId ?? -1
    )
    pendingSeekPreview = PendingSeekPreview(
      serial: seekSerial,
      requestId: requestId,
      targetPtsUs: targetPtsUs
    )
  }

  func resolvePendingSeekPreviewIfPresented(
    presentationState: MacOSFramePresentationState,
    emitSeekPreviewPresented: (Int?, Int) -> Void
  ) {
    guard let pending = pendingSeekPreview,
          pending.serial == seekRefreshSerial,
          let presentedPtsUs = presentationState.lastPresentedPtsUs else {
      return
    }
    guard MacOSNativeFrameRefresh.acceptsSeekPts(
      presentedPtsUs,
      targetPtsUs: pending.targetPtsUs
    ) else {
      MacOSProfilerLog.trace(
        "VoidPlayer macOS paused seek waiting for committed frame: targetPtsUs=\(pending.targetPtsUs) presentedPtsUs=\(presentedPtsUs) requestId=\(pending.requestId ?? -1)"
      )
      return
    }
    pendingSeekPreview = nil
    NSLog(
      "VoidPlayer macOS paused seek committed frame presented: targetPtsUs=%d presentedPtsUs=%d requestId=%d",
      pending.targetPtsUs,
      presentedPtsUs,
      pending.requestId ?? -1
    )
    emitSeekPreviewPresented(pending.requestId, pending.targetPtsUs)
  }

  func stepAndRefresh(
    forward: Bool,
    context: MacOSTransportContext
  ) -> FlutterError? {
    context.playback.stopForBlockingCommand(player: context.player, pausePlayer: false)
    guard context.nativeBackendActive,
          let player = context.player else {
      return nil
    }
    guard let texture = context.texture else {
      return nil
    }
    if let error = MacOSNativeFrameRefresh.stepAndRefresh(
      player: player,
      texture: texture,
      forward: forward,
      maxTrackSlots: context.maxTrackSlots,
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
    timeoutMs: Int,
    context: MacOSTransportContext
  ) -> MacOSNativeSeekRefreshResult {
    guard context.nativeBackendActive,
          let player = context.player,
          let texture = context.texture else {
      return .presented
    }

    return MacOSNativeFrameRefresh.seekAndRefresh(
      player: player,
      texture: texture,
      targetPtsUs: targetPtsUs,
      timeoutMs: timeoutMs,
      maxTrackSlots: context.maxTrackSlots,
      presentationState: context.presentationState,
      framePump: context.playback.framePumpForRefresh
    )
  }
}

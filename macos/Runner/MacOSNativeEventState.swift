import Foundation
import FlutterMacOS

final class MacOSNativeEventState {
  private var eventSink: FlutterEventSink?
  private var listenCount = 0
  private var emitCount = 0
  private var dropNoSinkCount = 0
  private var sequence = 0
  private var lastPlaybackClockEmitNs: UInt64 = 0
  private let playbackClockIntervalNs: UInt64 = 33_000_000

  func onListen(_ events: @escaping FlutterEventSink) {
    eventSink = events
    listenCount += 1
  }

  func onCancel() {
    eventSink = nil
  }

  func emitSeekPreviewPresented(
    requestId: Int?,
    targetPtsUs: Int,
    presentationState: MacOSFramePresentationState
  ) {
    guard let requestId,
          let ptsUs = presentationState.lastPresentedPtsUs else {
      return
    }
    guard let eventSink else {
      dropNoSinkCount += 1
      return
    }
    sequence += 1
    emitCount += 1
    let payload: [String: Any] = [
      "schemaVersion": 1,
      "sequence": sequence,
      "type": "seekPreviewPresented",
      "timestampUs": Int(Date().timeIntervalSince1970 * 1_000_000),
      "requestId": requestId,
      "trackFileId": 0,
      "ptsUs": ptsUs,
      "dtsUs": presentationState.lastPresentedDtsUs ?? ptsUs,
      "targetPtsUs": targetPtsUs,
    ]
    DispatchQueue.main.async {
      eventSink(payload)
    }
  }

  func emitRendererOwnedPresentationState(
    active: Bool,
    runnerLayerActive: Bool,
    rendererOwnedActive: Bool,
    requested: Bool,
    edrEnabled: Bool,
    mode: String,
    reason: String,
    failure: String
  ) {
    guard let eventSink else {
      dropNoSinkCount += 1
      return
    }
    sequence += 1
    emitCount += 1
    let payload: [String: Any] = [
      "schemaVersion": 1,
      "sequence": sequence,
      "type": "rendererOwnedPresentationState",
      "timestampUs": Int(Date().timeIntervalSince1970 * 1_000_000),
      "rendererOwnedPresentationActive": active,
      "rendererOwnedRunnerLayerActive": runnerLayerActive,
      "rendererOwnedRendererActive": rendererOwnedActive,
      "rendererOwnedPresentationRequested": requested,
      "rendererOwnedEDROutputEnabled": edrEnabled,
      "rendererOwnedPresentationMode": mode,
      "rendererOwnedPresentationReason": reason,
      "rendererOwnedPresentationFailure": failure,
    ]
    DispatchQueue.main.async {
      eventSink(payload)
    }
  }

  func emitPlaybackClock(
    currentPtsUs: Int,
    durationUs: Int,
    isPlaying: Bool,
    playbackSpeed: Double,
    force: Bool = false
  ) {
    let nowNs = DispatchTime.now().uptimeNanoseconds
    if !force,
       lastPlaybackClockEmitNs != 0,
       nowNs - lastPlaybackClockEmitNs < playbackClockIntervalNs {
      return
    }
    lastPlaybackClockEmitNs = nowNs
    guard let eventSink else {
      dropNoSinkCount += 1
      return
    }
    sequence += 1
    emitCount += 1
    let payload: [String: Any] = [
      "schemaVersion": 1,
      "sequence": sequence,
      "type": "playbackClock",
      "timestampUs": Int(nowNs / 1_000),
      "ptsUs": currentPtsUs,
      "durationUs": durationUs,
      "isPlaying": isPlaying,
      "playbackSpeed": playbackSpeed,
    ]
    DispatchQueue.main.async {
      eventSink(payload)
    }
  }

  func diagnosticMap() -> [String: Any] {
    [
      "nativeEventListenCount": listenCount,
      "nativeEventEmitCount": emitCount,
      "nativeEventDropNoSinkCount": dropNoSinkCount,
    ]
  }
}

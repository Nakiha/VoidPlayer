import Foundation
import FlutterMacOS

final class MacOSNativeEventState {
  private var eventSink: FlutterEventSink?
  private var listenCount = 0
  private var emitCount = 0
  private var dropNoSinkCount = 0
  private var sequence = 0

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

  func emitNativeCompositorState(
    active: Bool,
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
      "type": "nativeCompositorState",
      "timestampUs": Int(Date().timeIntervalSince1970 * 1_000_000),
      "nativeCompositorActive": active,
      "nativeCompositorRequested": requested,
      "nativeCompositorEDREnabled": edrEnabled,
      "nativeCompositorMode": mode,
      "nativeCompositorReason": reason,
      "nativeCompositorFailure": failure,
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

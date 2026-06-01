import FlutterMacOS
import Foundation

struct MacOSVideoTrackAddResult {
  let payload: Any
  let refreshCurrentFrame: Bool
  let markFrameAvailable: Bool
}

struct MacOSVideoTrackRemoveResult {
  let destroyPlayer: Bool
  let refreshCurrentFrame: Bool
}

final class MacOSVideoTrackController {
  private let store = MacOSVideoTrackStore()

  var tracks: [[String: Any]] {
    store.tracks
  }

  var count: Int {
    store.count
  }

  var isEmpty: Bool {
    store.isEmpty
  }

  var currentDurationUs: Int {
    store.currentDurationUs
  }

  func reset() {
    store.reset()
  }

  func replace(with tracks: [[String: Any]], fallbackDurationUs: Int) {
    store.replace(with: tracks, fallbackDurationUs: fallbackDurationUs)
  }

  func activeSlotCapacity() -> Int {
    store.activeSlotCapacity()
  }

  func addTrack(
    arguments: Any?,
    backendName: String,
    nativePlayer: MacOSNativePlayerSession?,
    textureDimensions: (width: Int, height: Int)?
  ) -> MacOSVideoTrackAddResult {
    let fileId = store.nextFileId()
    let slot = store.count
    guard let path = MacOSFlutterArguments.stringArg(arguments, "path"), !path.isEmpty else {
      return MacOSVideoTrackAddResult(
        payload: FlutterError(
          code: "INVALID_ARGUMENT",
          message: "addTrack requires a media path",
          details: nil
        ),
        refreshCurrentFrame: false,
        markFrameAvailable: false
      )
    }

    if backendName == MacOSVideoTrackPayload.nativeFormatName {
      do {
        guard let session = nativePlayer else {
          throw MacOSNativePlayerError.failed("macOS native player is unavailable")
        }
        let metadata = try session.addTrack(
          path: path,
          fileId: fileId,
          useHardwareDecode: MacOSFlutterArguments.boolArg(arguments, "useHardwareDecode") ?? true
        )
        let track = MacOSVideoTrackPayload.nativeTrack(
          path: path,
          metadata: metadata,
          decoderName: session.decoderName()
        )
        store.append(track)
        return MacOSVideoTrackAddResult(
          payload: track,
          refreshCurrentFrame: true,
          markFrameAvailable: false
        )
      } catch {
        return MacOSVideoTrackAddResult(
          payload: FlutterError(
            code: "DECODE_FAILED",
            message: "Failed to add macOS native track",
            details: "\(error)"
          ),
          refreshCurrentFrame: false,
          markFrameAvailable: false
        )
      }
    }

    let size = textureDimensions ?? (width: 1920, height: 1080)
    let track = MacOSVideoTrackPayload.syntheticTrack(
      fileId: fileId,
      slot: slot,
      path: path,
      width: size.width,
      height: size.height,
      durationUs: store.currentDurationUs
    )
    store.append(track)
    return MacOSVideoTrackAddResult(
      payload: track,
      refreshCurrentFrame: false,
      markFrameAvailable: true
    )
  }

  func removeTrack(
    arguments: Any?,
    backendName: String,
    nativePlayer: MacOSNativePlayerSession?
  ) -> MacOSVideoTrackRemoveResult {
    guard let fileId = MacOSFlutterArguments.intArg(arguments, "fileId") else {
      return MacOSVideoTrackRemoveResult(destroyPlayer: false, refreshCurrentFrame: false)
    }

    if backendName == MacOSVideoTrackPayload.nativeFormatName {
      nativePlayer?.removeTrack(fileId: fileId)
    }

    store.remove(
      fileId: fileId,
      compactSlots: backendName != MacOSVideoTrackPayload.nativeFormatName
    )
    return MacOSVideoTrackRemoveResult(
      destroyPlayer: store.isEmpty,
      refreshCurrentFrame: !store.isEmpty
    )
  }
}

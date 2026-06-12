import Foundation

struct MacOSNativeFrameInfo {
  let width: Int
  let height: Int
  let durationUs: Int
  let ptsUs: Int
  let dtsUs: Int
  let targetPixelBufferAddress: UInt
  let layoutRevision: UInt64
}

struct MacOSPendingNativeFrame {
  let info: MacOSNativeFrameInfo
  let publishToken: MacOSNativeFramePublishToken
}

struct MacOSNativeFramePublishToken {
  let pixelBufferAddress: UInt
  let nativeUploadCount: Int
}

struct MacOSNativeTrackMetadata {
  let fileId: Int
  let slot: Int
  let width: Int
  let height: Int
  let durationUs: Int
  let startTimeUs: Int
  let bitRate: Int
  let formatName: String
  let codecName: String
  let codecLongName: String
  let decoderName: String
  let colorRange: Int
  let colorMatrix: Int
  let colorTransfer: Int
  let colorPrimaries: Int

  var isHDR: Bool {
    colorTransfer == MacOSNativeColorTransfer.pq ||
      colorTransfer == MacOSNativeColorTransfer.hlg
  }
}

enum MacOSNativeColorTransfer {
  static let unknown = 0
  static let sdr = 1
  static let pq = 2
  static let hlg = 3
}

enum MacOSNativePlayerError: Error, CustomStringConvertible {
  case failed(String)
  case invalidPayload
  case transientFrameUnavailable(String)

  var description: String {
    switch self {
    case .failed(let message):
      return message
    case .invalidPayload:
      return "native frame had invalid dimensions or pixel data"
    case .transientFrameUnavailable(let message):
      return message
    }
  }

  var isTransientFrameUnavailable: Bool {
    switch self {
    case .failed(let message):
      return message == "no presentable frame is ready" ||
        message == "no decoded frame is ready" ||
        message == "not all present decision frames are ready" ||
        message == "timed out waiting for a decoded frame" ||
        message == "shared macOS renderer has not presented a frame yet" ||
        message == "renderer-owned Metal presentation target is not installed" ||
        message == "renderer-owned Metal presentation target changed during refresh"
    case .invalidPayload:
      return false
    case .transientFrameUnavailable:
      return true
    }
  }
}

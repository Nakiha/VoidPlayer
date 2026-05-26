import Foundation

struct MacOSNativeFrameInfo {
  let width: Int
  let height: Int
  let durationUs: Int
  let ptsUs: Int
  let dtsUs: Int
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
        message == "shared macOS renderer has not presented a frame yet"
    case .invalidPayload:
      return false
    case .transientFrameUnavailable:
      return true
    }
  }
}

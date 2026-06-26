import Foundation

struct MacOSNativeFrameInfo {
  let width: Int
  let height: Int
  let durationUs: Int
  let ptsUs: Int
  let dtsUs: Int
  let analysisFrameIndex: Int
  let frameIdentityMode: Int
  let sourcePacketIndex: Int
  let sourcePacketSize: Int
  let sourcePacketPos: Int64
  let sourcePacketPtsUs: Int64
  let sourcePacketDtsUs: Int64
  let targetPixelBufferAddress: UInt
  let layoutRevision: UInt64

  init(native info: VPMacOSNativeFrameInfo) {
    width = Int(info.width)
    height = Int(info.height)
    durationUs = Int(info.duration_us)
    ptsUs = Int(info.pts_us)
    dtsUs = Int(info.dts_us)
    analysisFrameIndex = Int(info.analysis_frame_index)
    frameIdentityMode = Int(info.frame_identity_mode)
    sourcePacketIndex = Int(info.source_packet_index)
    sourcePacketSize = Int(info.source_packet_size)
    sourcePacketPos = Int64(info.source_packet_pos)
    sourcePacketPtsUs = Int64(info.source_packet_pts)
    sourcePacketDtsUs = Int64(info.source_packet_dts)
    targetPixelBufferAddress = UInt(info.target_pixel_buffer_address)
    layoutRevision = UInt64(info.layout_revision)
  }

  func presentedFrameMap(fileId: Int? = nil) -> [String: Any] {
    var map: [String: Any] = [
      "ptsUs": Int64(ptsUs),
      "dtsUs": Int64(dtsUs),
      "durationUs": durationUs,
      "analysisFrameIndex": analysisFrameIndex,
      "frameIdentityMode": frameIdentityMode,
      "sourcePacketIndex": sourcePacketIndex,
      "sourcePacketSize": sourcePacketSize,
      "sourcePacketPos": sourcePacketPos,
      "sourcePacketPtsUs": sourcePacketPtsUs,
      "sourcePacketDtsUs": sourcePacketDtsUs,
    ]
    if let fileId {
      map["fileId"] = fileId
    }
    return map
  }
}

struct MacOSPendingNativeFrame {
  let info: MacOSNativeFrameInfo
  let publishToken: MacOSNativeFramePublishToken
}

struct MacOSNativeFramePublishToken {
  let pixelBufferAddress: UInt
  let nativeUploadCount: Int
  let pixelBufferGeneration: Int
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

import Foundation

enum MacOSVideoTrackPayload {
  static func track(
    fileId: Int,
    slot: Int,
    path: String,
    width: Int,
    height: Int,
    durationUs: Int,
    formatName: String,
    codecName: String,
    codecLongName: String,
    decoderName: String
  ) -> [String: Any] {
    return [
      "fileId": fileId,
      "slot": slot,
      "path": path,
      "width": width,
      "height": height,
      "durationUs": durationUs,
      "startTimeUs": 0,
      "bitRate": 0,
      "formatName": formatName,
      "codecName": codecName,
      "codecLongName": codecLongName,
      "decoderName": decoderName,
    ]
  }

  static func defaultLayout() -> [String: Any] {
    return [
      "mode": 0,
      "splitPos": 0.5,
      "zoomRatio": 1.0,
      "viewOffsetX": 0.0,
      "viewOffsetY": 0.0,
      "pixelSizeMode": 0,
      "order": [0, 1, 2, 3],
    ]
  }
}

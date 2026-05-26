import Foundation

enum MacOSVideoTrackPayload {
  static let syntheticDurationUs = 10_000_000
  static let nativeFormatName = "macos-native-player"
  static let nativeCodecName = "ffmpeg"
  static let nativeCodecLongName = "macOS shared native renderer pipeline"
  static let syntheticFormatName = "synthetic"
  static let syntheticCodecName = "macos_synthetic"
  static let syntheticCodecLongName = "macOS Synthetic FlutterTexture"
  static let syntheticDecoderName = "synthetic"

  static func track(
    fileId: Int,
    slot: Int,
    path: String,
    width: Int,
    height: Int,
    durationUs: Int,
    startTimeUs: Int = 0,
    bitRate: Int = 0,
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
      "startTimeUs": startTimeUs,
      "bitRate": bitRate,
      "formatName": formatName,
      "codecName": codecName,
      "codecLongName": codecLongName,
      "decoderName": decoderName,
    ]
  }

  static func nativeTrack(
    path: String,
    metadata: MacOSNativeTrackMetadata,
    decoderName: String
  ) -> [String: Any] {
    track(
      fileId: metadata.fileId,
      slot: metadata.slot,
      path: path,
      width: metadata.width,
      height: metadata.height,
      durationUs: metadata.durationUs,
      startTimeUs: metadata.startTimeUs,
      bitRate: metadata.bitRate,
      formatName: nonEmpty(metadata.formatName, nativeFormatName),
      codecName: nonEmpty(metadata.codecName, nativeCodecName),
      codecLongName: nonEmpty(metadata.codecLongName, nativeCodecLongName),
      decoderName: nonEmpty(metadata.decoderName, decoderName)
    )
  }

  static func syntheticTrack(
    fileId: Int,
    slot: Int,
    path: String,
    width: Int,
    height: Int,
    durationUs: Int
  ) -> [String: Any] {
    track(
      fileId: fileId,
      slot: slot,
      path: path,
      width: width,
      height: height,
      durationUs: durationUs,
      formatName: syntheticFormatName,
      codecName: syntheticCodecName,
      codecLongName: syntheticCodecLongName,
      decoderName: syntheticDecoderName
    )
  }

  private static func nonEmpty(_ value: String, _ fallback: String) -> String {
    value.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? fallback : value
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

import Foundation

final class MacOSNativePlayerSession {
  let handle: OpaquePointer

  init?() {
    guard let handle = VPMacOSNativePlayerCreate() else {
      return nil
    }
    self.handle = handle
  }

  deinit {
    setFrameAvailableCallback(nil, userData: nil)
    clearMetalPresentationTarget()
    VPMacOSNativePlayerDestroy(handle)
  }

  func open(path: String) throws {
    var error = [CChar](repeating: 0, count: 1024)
    let ret = path.withCString { pathPointer in
      VPMacOSNativePlayerOpen(handle, pathPointer, &error, error.count)
    }
    if ret != 0 {
      let message = String(cString: error)
      throw MacOSNativePlayerError.failed(
        message.isEmpty ? "macOS native player open failed with code \(ret)" : message
      )
    }
  }

  func addTrack(path: String, fileId: Int) throws -> MacOSNativeTrackMetadata {
    var info = VPMacOSNativeTrackInfo()
    var error = [CChar](repeating: 0, count: 1024)
    let ret = path.withCString { pathPointer in
      VPMacOSNativePlayerAddTrack(handle, pathPointer, Int32(fileId), &info, &error, error.count)
    }
    if ret != 0 {
      let message = String(cString: error)
      throw MacOSNativePlayerError.failed(
        message.isEmpty ? "macOS native player addTrack failed with code \(ret)" : message
      )
    }
    return Self.trackMetadata(from: info)
  }

  func trackMetadata(fileId: Int) throws -> MacOSNativeTrackMetadata {
    var info = VPMacOSNativeTrackInfo()
    let ret = VPMacOSNativePlayerCopyTrackInfo(handle, Int32(fileId), &info)
    if ret != 0 {
      throw MacOSNativePlayerError.failed(
        "macOS native player did not report track \(fileId)"
      )
    }
    return Self.trackMetadata(from: info)
  }

  func removeTrack(fileId: Int) {
    VPMacOSNativePlayerRemoveTrack(handle, Int32(fileId))
  }

  func close() {
    VPMacOSNativePlayerClose(handle)
  }

  private static func trackMetadata(from info: VPMacOSNativeTrackInfo)
    -> MacOSNativeTrackMetadata {
    MacOSNativeTrackMetadata(
      fileId: Int(info.file_id),
      slot: Int(info.slot),
      width: Int(info.width),
      height: Int(info.height),
      durationUs: Int(info.duration_us),
      startTimeUs: Int(info.start_time_us),
      bitRate: Int(info.bit_rate),
      formatName: cString(info.format_name),
      codecName: cString(info.codec_name),
      codecLongName: cString(info.codec_long_name),
      decoderName: cString(info.decoder_name)
    )
  }

  private static func cString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { rawBuffer -> String in
      guard let base = rawBuffer.bindMemory(to: CChar.self).baseAddress else {
        return ""
      }
      return String(cString: base)
    }
  }
}

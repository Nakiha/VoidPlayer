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
    return MacOSNativeTrackMetadata(
      fileId: Int(info.file_id),
      slot: Int(info.slot),
      width: Int(info.width),
      height: Int(info.height),
      durationUs: Int(info.duration_us)
    )
  }

  func removeTrack(fileId: Int) {
    VPMacOSNativePlayerRemoveTrack(handle, Int32(fileId))
  }

  func close() {
    VPMacOSNativePlayerClose(handle)
  }

}

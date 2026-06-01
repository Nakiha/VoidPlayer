import CoreVideo
import Foundation

extension MacOSNativePlayerSession {
  func setFrameAvailableCallback(
    _ callback: VPMacOSFrameAvailableCallback?,
    userData: UnsafeMutableRawPointer?
  ) {
    VPMacOSNativePlayerSetFrameAvailableCallback(handle, callback, userData)
  }

  func setMetalPresentationTarget(
    backend: OpaquePointer,
    pixelBuffer: CVPixelBuffer,
    width: Int,
    height: Int,
    maxTrackSlots: Int,
    refresh: Bool = true
  ) -> Bool {
    let pixelBufferPointer =
      UnsafeMutableRawPointer(Unmanaged.passUnretained(pixelBuffer).toOpaque())
    let clampedTrackSlots = Int32(max(1, min(4, maxTrackSlots)))
    if refresh {
      return VPMacOSNativePlayerSetMetalPresentationTarget(
        handle,
        backend,
        pixelBufferPointer,
        Int32(width),
        Int32(height),
        clampedTrackSlots
      ) == 0
    }
    return VPMacOSNativePlayerInstallMetalPresentationTarget(
      handle,
      backend,
      pixelBufferPointer,
      Int32(width),
      Int32(height),
      clampedTrackSlots
    ) == 0
  }

  func installMetalPresentationTargetRing(
    backend: OpaquePointer,
    pixelBuffers: [CVPixelBuffer],
    displayedPixelBuffer: CVPixelBuffer?,
    protectedPixelBuffer: CVPixelBuffer?,
    width: Int,
    height: Int,
    maxTrackSlots: Int
  ) -> Bool {
    guard !pixelBuffers.isEmpty else { return false }
    var pixelBufferPointers: [UnsafeRawPointer?] = pixelBuffers.map {
      UnsafeRawPointer(Unmanaged.passUnretained($0).toOpaque())
    }
    let displayedPointer = displayedPixelBuffer.map {
      UnsafeMutableRawPointer(Unmanaged.passUnretained($0).toOpaque())
    }
    let protectedPointer = protectedPixelBuffer.map {
      UnsafeMutableRawPointer(Unmanaged.passUnretained($0).toOpaque())
    }
    let clampedTrackSlots = Int32(max(1, min(4, maxTrackSlots)))
    return pixelBufferPointers.withUnsafeMutableBufferPointer { buffer in
      VPMacOSNativePlayerInstallMetalPresentationTargetRing(
        handle,
        backend,
        buffer.baseAddress,
        buffer.count,
        displayedPointer,
        protectedPointer,
        Int32(width),
        Int32(height),
        clampedTrackSlots
      ) == 0
    }
  }

  func markMetalPresentationTargetDisplayed(_ pixelBuffer: CVPixelBuffer) {
    VPMacOSNativePlayerMarkMetalPresentationTargetDisplayed(
      handle,
      UnsafeMutableRawPointer(Unmanaged.passUnretained(pixelBuffer).toOpaque())
    )
  }

  func protectMetalPresentationTarget(_ pixelBuffer: CVPixelBuffer?) {
    let pointer = pixelBuffer.map {
      UnsafeMutableRawPointer(Unmanaged.passUnretained($0).toOpaque())
    }
    VPMacOSNativePlayerProtectMetalPresentationTarget(handle, pointer)
  }

  func releaseMetalPresentationTarget(_ pixelBuffer: CVPixelBuffer) {
    VPMacOSNativePlayerReleaseMetalPresentationTarget(
      handle,
      UnsafeMutableRawPointer(Unmanaged.passUnretained(pixelBuffer).toOpaque())
    )
  }

  func clearMetalPresentationTarget() {
    VPMacOSNativePlayerClearMetalPresentationTarget(handle)
  }

  func rendererOwnedPresentationActive() -> Bool {
    VPMacOSNativePlayerRendererOwnedPresentationActive(handle) != 0
  }

  func lastRendererOwnedPresentationSucceeded() -> Bool {
    VPMacOSNativePlayerLastRendererOwnedPresentationSucceeded(handle) != 0
  }

  func lastRendererOwnedFrameInfo() -> MacOSNativeFrameInfo? {
    var info = VPMacOSNativeFrameInfo()
    guard VPMacOSNativePlayerCopyLastRendererOwnedFrameInfo(handle, &info) == 0 else {
      return nil
    }
    return MacOSNativeFrameInfo(
      width: Int(info.width),
      height: Int(info.height),
      durationUs: Int(info.duration_us),
      ptsUs: Int(info.pts_us),
      dtsUs: Int(info.dts_us),
      targetPixelBufferAddress: UInt(info.target_pixel_buffer_address)
    )
  }

  func copyLastRendererOwnedFrameInfo() throws -> MacOSNativeFrameInfo {
    var info = VPMacOSNativeFrameInfo()
    var error = [CChar](repeating: 0, count: 1024)
    let ret = VPMacOSNativePlayerPresentCurrentFrameToMetalTarget(
      handle,
      &info,
      &error,
      error.count
    )
    if ret != 0 {
      let message = String(cString: error)
      throw MacOSNativePlayerError.failed(
        message.isEmpty
          ? "macOS renderer-owned presentation failed with code \(ret)"
          : message
      )
    }
    guard info.width > 0, info.height > 0 else {
      throw MacOSNativePlayerError.invalidPayload
    }
    return MacOSNativeFrameInfo(
      width: Int(info.width),
      height: Int(info.height),
      durationUs: Int(info.duration_us),
      ptsUs: Int(info.pts_us),
      dtsUs: Int(info.dts_us),
      targetPixelBufferAddress: UInt(info.target_pixel_buffer_address)
    )
  }

  func requestRendererOwnedFrameRefresh(timeoutMs: Int) throws -> MacOSNativeFrameInfo {
    var info = VPMacOSNativeFrameInfo()
    var error = [CChar](repeating: 0, count: 1024)
    let ret = VPMacOSNativePlayerRequestRendererOwnedFrameRefresh(
      handle,
      Int32(max(0, timeoutMs)),
      &info,
      &error,
      error.count
    )
    if ret != 0 {
      let message = String(cString: error)
      let fallback = ret == -2
        ? "renderer-owned Metal frame refresh timed out"
        : "macOS renderer-owned presentation failed with code \(ret)"
      let finalMessage = message.isEmpty ? fallback : message
      if ret == -2 {
        throw MacOSNativePlayerError.transientFrameUnavailable(finalMessage)
      }
      throw MacOSNativePlayerError.failed(finalMessage)
    }
    guard info.width > 0, info.height > 0 else {
      throw MacOSNativePlayerError.invalidPayload
    }
    return MacOSNativeFrameInfo(
      width: Int(info.width),
      height: Int(info.height),
      durationUs: Int(info.duration_us),
      ptsUs: Int(info.pts_us),
      dtsUs: Int(info.dts_us),
      targetPixelBufferAddress: UInt(info.target_pixel_buffer_address)
    )
  }

  func resetRendererOwnedPresentationStats() {
    VPMacOSNativePlayerResetRendererOwnedPresentationStats(handle)
  }

  func rendererOwnedPresentationUploadCount() -> Int {
    Int(VPMacOSNativePlayerRendererOwnedPresentationUploadCount(handle))
  }

  func rendererOwnedPresentationFailureCount() -> Int {
    Int(VPMacOSNativePlayerRendererOwnedPresentationFailureCount(handle))
  }
}

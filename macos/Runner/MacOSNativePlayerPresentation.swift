import CoreVideo
import Foundation
import Metal

extension MacOSNativePlayerSession {
  func setFrameAvailableCallback(
    _ callback: VPMacOSFrameAvailableCallback?,
    userData: UnsafeMutableRawPointer?
  ) {
    VPMacOSNativePlayerSetFrameAvailableCallback(handle, callback, userData)
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

  func installMetalDrawableTarget(
    backend: OpaquePointer,
    texture: MTLTexture,
    texturePointer: UnsafeMutableRawPointer,
    width: Int,
    height: Int,
    maxTrackSlots: Int,
    viewportLeft: Float = 0.0,
    viewportTop: Float = 0.0,
    viewportRight: Float = 1.0,
    viewportBottom: Float = 1.0
  ) -> Bool {
    let clampedTrackSlots = Int32(max(1, min(4, maxTrackSlots)))
    return VPMacOSNativePlayerInstallMetalDrawableTarget(
      handle,
      backend,
      texturePointer,
      Int32(width),
      Int32(height),
      UInt64(texture.pixelFormat.rawValue),
      clampedTrackSlots,
      viewportLeft,
      viewportTop,
      viewportRight,
      viewportBottom
    ) == 0
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

  func updateExternalFlutterSurface(
    texture: MTLTexture,
    frameGeneration: UInt64
  ) -> Bool {
    VPMacOSNativePlayerUpdateExternalFlutterSurface(
      handle,
      Unmanaged.passUnretained(texture as AnyObject).toOpaque(),
      Int32(texture.width),
      Int32(texture.height),
      UInt64(texture.pixelFormat.rawValue),
      frameGeneration
    ) == 0
  }

  func clearExternalFlutterSurface() {
    VPMacOSNativePlayerClearExternalFlutterSurface(handle)
  }

  func updateSourceProjection(
    mode: Int,
    splitPos: Double,
    activeTrackCount: Int,
    order: [Int],
    displayOffsetX: [Double],
    displayOffsetY: [Double],
    invDisplaySizeX: [Double],
    invDisplaySizeY: [Double],
    viewOffsetUvX: [Double],
    viewOffsetUvY: [Double]
  ) -> Bool {
    func orderValue(_ index: Int) -> Int32 {
      Int32(order.indices.contains(index) ? order[index] : index)
    }
    func floatValue(_ values: [Double], _ index: Int) -> Float {
      Float(values.indices.contains(index) && values[index].isFinite ? values[index] : 0.0)
    }
    let sourceOrder = (0..<4).map(orderValue)
    let displayOffsetXValues = (0..<4).map { floatValue(displayOffsetX, $0) }
    let displayOffsetYValues = (0..<4).map { floatValue(displayOffsetY, $0) }
    let invDisplaySizeXValues = (0..<4).map { floatValue(invDisplaySizeX, $0) }
    let invDisplaySizeYValues = (0..<4).map { floatValue(invDisplaySizeY, $0) }
    let viewOffsetUvXValues = (0..<4).map { floatValue(viewOffsetUvX, $0) }
    let viewOffsetUvYValues = (0..<4).map { floatValue(viewOffsetUvY, $0) }
    return sourceOrder.withUnsafeBufferPointer { sourceOrderPointer in
      displayOffsetXValues.withUnsafeBufferPointer { displayOffsetXPointer in
        displayOffsetYValues.withUnsafeBufferPointer { displayOffsetYPointer in
          invDisplaySizeXValues.withUnsafeBufferPointer { invDisplaySizeXPointer in
            invDisplaySizeYValues.withUnsafeBufferPointer { invDisplaySizeYPointer in
              viewOffsetUvXValues.withUnsafeBufferPointer { viewOffsetUvXPointer in
                viewOffsetUvYValues.withUnsafeBufferPointer { viewOffsetUvYPointer in
                  VPMacOSNativePlayerUpdateSourceProjection(
                    handle,
                    Int32(mode),
                    Float(splitPos.isFinite ? min(1.0, max(0.0, splitPos)) : 0.5),
                    Int32(max(1, min(4, activeTrackCount))),
                    sourceOrderPointer.baseAddress,
                    displayOffsetXPointer.baseAddress,
                    displayOffsetYPointer.baseAddress,
                    invDisplaySizeXPointer.baseAddress,
                    invDisplaySizeYPointer.baseAddress,
                    viewOffsetUvXPointer.baseAddress,
                    viewOffsetUvYPointer.baseAddress,
                    4
                  ) == 0
                }
              }
            }
          }
        }
      }
    }
  }

  func clearSourceProjection() {
    VPMacOSNativePlayerClearSourceProjection(handle)
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
    return MacOSNativeFrameInfo(native: info)
  }

  func requestRendererOwnedFrameRefresh(
    timeoutMs: Int,
    suppressFrameCallback: Bool = false
  ) throws -> MacOSNativeFrameInfo {
    var info = VPMacOSNativeFrameInfo()
    var error = [CChar](repeating: 0, count: 1024)
    let ret = VPMacOSNativePlayerRequestRendererOwnedFrameRefreshWithOptions(
      handle,
      Int32(max(0, timeoutMs)),
      suppressFrameCallback ? UInt32(VPMacOSNativeFrameRefreshSuppressFrameCallback) : 0,
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
    return MacOSNativeFrameInfo(native: info)
  }

  func submitRendererOwnedFrameRefresh() throws {
    var error = [CChar](repeating: 0, count: 1024)
    let ret = VPMacOSNativePlayerSubmitRendererOwnedFrameRefresh(
      handle,
      &error,
      error.count
    )
    guard ret == 0 else {
      let message = String(cString: error)
      let fallback = ret == -2
        ? "renderer-owned Metal frame refresh deferred by backpressure"
        : "macOS renderer-owned presentation submit failed with code \(ret)"
      let finalMessage = message.isEmpty ? fallback : message
      if ret == -2 {
        throw MacOSNativePlayerError.transientFrameUnavailable(finalMessage)
      }
      throw MacOSNativePlayerError.failed(finalMessage)
    }
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

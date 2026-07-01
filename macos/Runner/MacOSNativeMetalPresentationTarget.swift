import CoreVideo
import Foundation
import Metal

final class MacOSNativeMetalPresentationTarget {
  private var backend: OpaquePointer?

  init(width: Int, height: Int) {
    recreate(width: width, height: height)
  }

  deinit {
    if let backend {
      VPMacOSMetalPresentationBackendDestroy(backend)
    }
  }

  func resize(width: Int, height: Int) {
    recreate(width: width, height: height)
  }

  func isAvailable() -> Bool {
    guard let backend else { return false }
    return VPMacOSMetalPresentationBackendIsAvailable(backend) != 0
  }

  func installRing(
    player: MacOSNativePlayerSession,
    pixelBuffers: [CVPixelBuffer],
    displayedPixelBuffer: CVPixelBuffer?,
    protectedPixelBuffer: CVPixelBuffer?,
    width: Int,
    height: Int,
    maxTrackSlots: Int
  ) -> Bool {
    guard let backend, isAvailable() else { return false }
    return player.installMetalPresentationTargetRing(
      backend: backend,
      pixelBuffers: pixelBuffers,
      displayedPixelBuffer: displayedPixelBuffer,
      protectedPixelBuffer: protectedPixelBuffer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots
    )
  }

  func installDrawable(
    player: MacOSNativePlayerSession,
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
    guard let backend, isAvailable() else { return false }
    return player.installMetalDrawableTarget(
      backend: backend,
      texture: texture,
      texturePointer: texturePointer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots,
      viewportLeft: viewportLeft,
      viewportTop: viewportTop,
      viewportRight: viewportRight,
      viewportBottom: viewportBottom
    )
  }

  func validate(pixelBuffer: CVPixelBuffer, width: Int, height: Int) -> (valid: Bool, error: String) {
    guard let backend else {
      return (valid: false, error: "native Metal presentation backend is null")
    }
    var error = [CChar](repeating: 0, count: 512)
    let status = VPMacOSMetalPresentationBackendValidatePixelBufferChecked(
      backend,
      UnsafeMutableRawPointer(Unmanaged.passUnretained(pixelBuffer).toOpaque()),
      Int32(width),
      Int32(height),
      &error,
      error.count
    )
    return status == 0
      ? (valid: true, error: "")
      : (valid: false, error: String(cString: error))
  }

  func bakeCurrentFrameSources(
    player: MacOSNativePlayerSession,
    targets: UnsafeMutableBufferPointer<VPMacOSNativeSourceFrameBakeTarget>
  ) -> (drawnCount: Int, error: String) {
    guard let backend else {
      return (drawnCount: -1, error: "native Metal source bake backend is null")
    }
    var error = [CChar](repeating: 0, count: 512)
    let ret = VPMacOSNativePlayerBakeCurrentFrameSources(
      player.handle,
      backend,
      targets.baseAddress,
      targets.count,
      &error,
      error.count
    )
    return ret >= 0
      ? (drawnCount: Int(ret), error: "")
      : (drawnCount: Int(ret), error: String(cString: error))
  }

  private func recreate(width: Int, height: Int) {
    if let backend {
      VPMacOSMetalPresentationBackendDestroy(backend)
    }
    backend = VPMacOSMetalPresentationBackendCreate(Int32(width), Int32(height))
  }
}

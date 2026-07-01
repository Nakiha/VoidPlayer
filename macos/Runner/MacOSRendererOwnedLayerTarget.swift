import AppKit
import CoreVideo
import Metal
import QuartzCore

private final class MacOSRendererOwnedMetalLayerView: NSView {
  let metalLayer = CAMetalLayer()

  init(device: MTLDevice, pixelFormat: MTLPixelFormat) {
    super.init(frame: .zero)
    wantsLayer = true
    isHidden = true
    layer = metalLayer
    metalLayer.device = device
    metalLayer.pixelFormat = pixelFormat
    metalLayer.framebufferOnly = false
    if #available(macOS 10.13.2, *) {
      metalLayer.maximumDrawableCount = 3
    }
    metalLayer.isOpaque = true
    metalLayer.contentsScale = NSScreen.main?.backingScaleFactor ?? 2.0
    if #available(macOS 10.15, *) {
      metalLayer.wantsExtendedDynamicRangeContent = true
    }
    if let colorSpace = CGColorSpace(name: CGColorSpace.extendedLinearDisplayP3) {
      metalLayer.colorspace = colorSpace
    }
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) {
    fatalError("init(coder:) has not been implemented")
  }

  override func hitTest(_ point: NSPoint) -> NSView? {
    nil
  }
}

private struct MacOSPendingRendererOwnedDrawable {
  let drawable: CAMetalDrawable
  let textureObject: AnyObject
}

final class MacOSRendererOwnedLayerTarget: MacOSRendererOwnedPresentationTarget {
  private let lock = NSLock()
  private weak var contentView: NSView?
  private let presentationTarget: MacOSNativeMetalPresentationTarget
  private let device: MTLDevice
  private let view: MacOSRendererOwnedMetalLayerView
  private var width: Int
  private var height: Int
  private var viewportLeft: Float = 0.0
  private var viewportTop: Float = 0.0
  private var viewportRight: Float = 1.0
  private var viewportBottom: Float = 1.0
  private var targetGeneration = 0
  private var lastPublishedNativeUploadCount = 0
  private var lastIgnoredNativeUploadCount = 0
  private var uploadCount = 0
  private var uploadFailureCount = 0
  private var viewportRectReady = false
  private var firstPresentBlockedUntilViewportCount = 0
  private var firstPresented = false
  private var firstPresentViewportLeft: Float = -1.0
  private var firstPresentViewportTop: Float = -1.0
  private var firstPresentViewportRight: Float = -1.0
  private var firstPresentViewportBottom: Float = -1.0
  private var pendingDrawables: [UInt: MacOSPendingRendererOwnedDrawable] = [:]

  var rendererOwnedRunnerLayerActive: Bool {
    true
  }

  init?(nativeWidth: Int, nativeHeight: Int, contentView: NSView?) {
    guard let device = MTLCreateSystemDefaultDevice(),
          let contentView else {
      return nil
    }
    self.contentView = contentView
    self.device = device
    self.width = nativeWidth
    self.height = nativeHeight
    self.presentationTarget = MacOSNativeMetalPresentationTarget(
      width: nativeWidth,
      height: nativeHeight
    )
    self.view = MacOSRendererOwnedMetalLayerView(
      device: device,
      pixelFormat: .rgba16Float
    )
    installView(into: contentView)
  }

  deinit {
    runOnMain {
      self.view.removeFromSuperview()
    }
  }

  func resize(width: Int, height: Int) -> Bool {
    updateFullSurfaceOnMain()
    return false
  }

  func prewarmRendererTarget(width: Int, height: Int) {}

  func setRendererTargetPixelFormat(
    _ nextPixelFormat: OSType,
    player: MacOSNativePlayerSession?
  ) -> Bool {
    nextPixelFormat == kCVPixelFormatType_64RGBAHalf
  }

  func updateFromNativePlayer(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    waitTimeoutMs: Int
  ) throws -> MacOSNativeFrameInfo {
    let pending = try drawFromNativePlayer(
      player,
      maxTrackSlots: maxTrackSlots,
      waitTimeoutMs: waitTimeoutMs
    )
    _ = try publishPendingNativeFrame(
      pending,
      player: player,
      maxTrackSlots: maxTrackSlots
    )
    return pending.info
  }

  func drawFromNativePlayer(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    waitTimeoutMs: Int
  ) throws -> MacOSPendingNativeFrame {
    let token = try installNextDrawable(player: player, maxTrackSlots: maxTrackSlots)
    do {
      let info = try player.requestRendererOwnedFrameRefresh(
        timeoutMs: waitTimeoutMs,
        suppressFrameCallback: true
      )
      let publishAddress = info.targetPixelBufferAddress
      guard publishAddress == token.pixelBufferAddress ||
              hasPendingDrawable(address: publishAddress) else {
        discardDrawable(address: token.pixelBufferAddress)
        throw MacOSNativePlayerError.transientFrameUnavailable(
          "renderer-owned Metal presentation target changed during refresh"
        )
      }
      if publishAddress != token.pixelBufferAddress {
        discardDrawable(address: token.pixelBufferAddress)
      }
      return MacOSPendingNativeFrame(
        info: info,
        publishToken: MacOSNativeFramePublishToken(
          pixelBufferAddress: publishAddress,
          nativeUploadCount: player.rendererOwnedPresentationUploadCount(),
          pixelBufferGeneration: currentTargetGeneration()
        )
      )
    } catch {
      discardDrawable(address: token.pixelBufferAddress)
      uploadFailureCount += 1
      throw error
    }
  }

  func publishPendingNativeFrame(
    _ pending: MacOSPendingNativeFrame,
    player: MacOSNativePlayerSession,
    maxTrackSlots: Int
  ) throws -> MacOSNativeFramePublishOutcome {
    lock.lock()
    guard pending.publishToken.pixelBufferGeneration == targetGeneration else {
      lastIgnoredNativeUploadCount = max(
        lastIgnoredNativeUploadCount,
        pending.publishToken.nativeUploadCount
      )
      lock.unlock()
      discardDrawable(address: pending.publishToken.pixelBufferAddress)
      throw MacOSNativePlayerError.transientFrameUnavailable(
        "renderer-owned Metal presentation target generation changed before publish"
      )
    }
    let callbackUploadFloor = max(
      lastPublishedNativeUploadCount,
      lastIgnoredNativeUploadCount
    )
    guard pending.publishToken.nativeUploadCount > callbackUploadFloor else {
      lock.unlock()
      discardDrawable(address: pending.publishToken.pixelBufferAddress)
      return .alreadyPublished
    }
    lock.unlock()
    guard presentDrawable(address: pending.publishToken.pixelBufferAddress) else {
      lock.lock()
      lastIgnoredNativeUploadCount = max(
        lastIgnoredNativeUploadCount,
        pending.publishToken.nativeUploadCount
      )
      lock.unlock()
      return .notReady
    }
    lock.lock()
    lastPublishedNativeUploadCount = pending.publishToken.nativeUploadCount
    uploadCount += 1
    lock.unlock()
    return .published
  }

  func discardPendingNativeFrame(_ pending: MacOSPendingNativeFrame) {
    discardDrawable(address: pending.publishToken.pixelBufferAddress)
  }

  func rendererOwnedTargetDiagnostics() -> [String: Any] {
    lock.lock()
    defer { lock.unlock() }
    return [
      "rendererOwnedTargetPixelFormat": "CAMetalLayerRGBA16Float",
      "rendererOwnedEDROutputEnabled": true,
      "rendererOwnedEDRTargetSampleCount": 0,
      "rendererOwnedEDRTargetMaxRGBX1000": 0,
      "rendererOwnedEDRTargetPixelsOver1X1000": 0,
      "rendererOwnedLayerTargetActive": true,
      "rendererOwnedLayerPendingDrawableCount": pendingDrawables.count,
      "rendererOwnedLayerUploadCount": uploadCount,
      "rendererOwnedLayerUploadFailureCount": uploadFailureCount,
      "rendererOwnedLayerTargetGeneration": targetGeneration,
      "rendererOwnedLayerViewportRectReady": viewportRectReady,
      "rendererOwnedLayerFirstPresentBlockedUntilViewportCount":
        firstPresentBlockedUntilViewportCount,
      "rendererOwnedLayerLastPublishedUploadCount": lastPublishedNativeUploadCount,
      "rendererOwnedLayerLastIgnoredUploadCount": lastIgnoredNativeUploadCount,
      "rendererOwnedLayerFirstPresented": firstPresented,
      "rendererOwnedLayerFirstPresentViewportLeftPermille":
        Int(firstPresentViewportLeft * 1000.0),
      "rendererOwnedLayerFirstPresentViewportTopPermille":
        Int(firstPresentViewportTop * 1000.0),
      "rendererOwnedLayerFirstPresentViewportRightPermille":
        Int(firstPresentViewportRight * 1000.0),
      "rendererOwnedLayerFirstPresentViewportBottomPermille":
        Int(firstPresentViewportBottom * 1000.0),
    ]
  }

  func setRendererOwnedViewportRect(
    left: Int,
    top: Int,
    width: Int,
    height: Int,
    surfaceWidth: Int,
    surfaceHeight: Int
  ) {
    guard width > 0, height > 0, surfaceWidth > 0, surfaceHeight > 0 else {
      return
    }
    let nextSurfaceWidth = max(16, surfaceWidth)
    let nextSurfaceHeight = max(16, surfaceHeight)
    let nextLeft = Float(max(0, min(left, surfaceWidth))) / Float(max(1, surfaceWidth))
    let nextTop = Float(max(0, min(top, surfaceHeight))) / Float(max(1, surfaceHeight))
    let nextRight = Float(max(0, min(left + width, surfaceWidth))) / Float(max(1, surfaceWidth))
    let nextBottom = Float(max(0, min(top + height, surfaceHeight))) / Float(max(1, surfaceHeight))
    lock.lock()
    let changed = !viewportRectReady ||
      nextSurfaceWidth != self.width ||
      nextSurfaceHeight != self.height ||
      nextLeft != viewportLeft ||
      nextTop != viewportTop ||
      nextRight != viewportRight ||
      nextBottom != viewportBottom
    viewportRectReady = true
    if changed {
      self.width = nextSurfaceWidth
      self.height = nextSurfaceHeight
      viewportLeft = nextLeft
      viewportTop = nextTop
      viewportRight = max(nextLeft, nextRight)
      viewportBottom = max(nextTop, nextBottom)
      targetGeneration &+= 1
      lastPublishedNativeUploadCount = 0
      lastIgnoredNativeUploadCount = 0
      pendingDrawables.removeAll()
    }
    lock.unlock()
    guard changed else {
      updateFullSurfaceOnMain()
      return
    }
    presentationTarget.resize(width: nextSurfaceWidth, height: nextSurfaceHeight)
    updateFullSurfaceOnMain()
  }

  func installNativePresentationTarget(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    refresh: Bool
  ) -> Bool {
    // CAMetalDrawable is a leased presentation object, not a reusable render
    // target pool. Installing one before native is about to render keeps it
    // live in CAMetalLayer and can exhaust the layer queue during startup or
    // playback callbacks. The layer target installs a drawable only inside
    // drawFromNativePlayer(), where the drawable is immediately rendered and
    // either presented or discarded.
    presentationTarget.isAvailable()
  }

  func resetNativeUploadBaseline() {
    lock.lock()
    lastPublishedNativeUploadCount = 0
    lastIgnoredNativeUploadCount = 0
    lock.unlock()
  }

  func publishRenderedTargetAndInstallNext(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    frameInfo: MacOSNativeFrameInfo?
  ) -> Bool {
    let nativeUploadCount = player.rendererOwnedPresentationUploadCount()
    lock.lock()
    if nativeUploadCount <= max(lastPublishedNativeUploadCount, lastIgnoredNativeUploadCount) {
      lock.unlock()
      return false
    }
    lock.unlock()
    guard presentDrawable(address: frameInfo?.targetPixelBufferAddress ?? 0) else {
      lock.lock()
      lastIgnoredNativeUploadCount = max(lastIgnoredNativeUploadCount, nativeUploadCount)
      lock.unlock()
      return false
    }
    lock.lock()
    lastPublishedNativeUploadCount = nativeUploadCount
    uploadCount += 1
    lock.unlock()
    return true
  }

  private func installNextDrawable(
    player: MacOSNativePlayerSession,
    maxTrackSlots: Int
  ) throws -> MacOSNativeFramePublishToken {
    guard presentationTarget.isAvailable() else {
      throw MacOSNativePlayerError.failed(
        "renderer-owned Metal presentation backend is unavailable"
      )
    }
    guard let drawable = nextDrawableOnMain() else {
      throw MacOSNativePlayerError.transientFrameUnavailable(
        "renderer-owned Metal drawable is unavailable"
      )
    }
    let texture = drawable.texture
    let textureObject = texture as AnyObject
    let texturePointer = Unmanaged.passUnretained(textureObject).toOpaque()
    lock.lock()
    let currentViewportLeft = viewportLeft
    let currentViewportTop = viewportTop
    let currentViewportRight = viewportRight
    let currentViewportBottom = viewportBottom
    lock.unlock()
    let installed = presentationTarget.installDrawable(
      player: player,
      texture: texture,
      texturePointer: texturePointer,
      width: texture.width,
      height: texture.height,
      maxTrackSlots: maxTrackSlots,
      viewportLeft: currentViewportLeft,
      viewportTop: currentViewportTop,
      viewportRight: currentViewportRight,
      viewportBottom: currentViewportBottom
    )
    guard installed else {
      throw MacOSNativePlayerError.failed(
        "failed to install renderer-owned Metal drawable target"
      )
    }
    let address = UInt(bitPattern: texturePointer)
    lock.lock()
    pendingDrawables[address] = MacOSPendingRendererOwnedDrawable(
      drawable: drawable,
      textureObject: textureObject
    )
    let generation = targetGeneration
    lock.unlock()
    return MacOSNativeFramePublishToken(
      pixelBufferAddress: address,
      nativeUploadCount: player.rendererOwnedPresentationUploadCount(),
      pixelBufferGeneration: generation
    )
  }

  @discardableResult
  private func presentDrawable(address: UInt) -> Bool {
    guard address != 0 else { return false }
    lock.lock()
    let pending = pendingDrawables.removeValue(forKey: address)
    guard let pending else {
      lock.unlock()
      return false
    }
    if !firstPresented && !viewportRectReady {
      firstPresentBlockedUntilViewportCount += 1
      lock.unlock()
      return false
    }
    if !firstPresented {
      firstPresentViewportLeft = viewportLeft
      firstPresentViewportTop = viewportTop
      firstPresentViewportRight = viewportRight
      firstPresentViewportBottom = viewportBottom
      firstPresented = true
    }
    lock.unlock()
    runOnMain {
      self.view.isHidden = false
    }
    pending.drawable.present()
    return true
  }

  private func discardDrawable(address: UInt) {
    guard address != 0 else { return }
    lock.lock()
    pendingDrawables.removeValue(forKey: address)
    lock.unlock()
  }

  private func hasPendingDrawable(address: UInt) -> Bool {
    guard address != 0 else { return false }
    lock.lock()
    let exists = pendingDrawables[address] != nil
    lock.unlock()
    return exists
  }

  private func currentTargetGeneration() -> Int {
    lock.lock()
    let generation = targetGeneration
    lock.unlock()
    return generation
  }

  private func nextDrawableOnMain() -> CAMetalDrawable? {
    runOnMain {
      view.metalLayer.nextDrawable()
    }
  }

  private func installView(into contentView: NSView) {
    runOnMain {
      contentView.addSubview(self.view, positioned: .above, relativeTo: nil)
      self.updateFullSurfaceOnMainLocked(contentView: contentView)
    }
  }

  private func updateFullSurfaceOnMain() {
    runOnMain {
      guard let contentView = self.contentView else { return }
      if self.view.superview == nil {
        contentView.addSubview(self.view, positioned: .above, relativeTo: nil)
      }
      self.updateFullSurfaceOnMainLocked(contentView: contentView)
    }
  }

  private func updateFullSurfaceOnMainLocked(contentView: NSView) {
    let scale = contentView.window?.backingScaleFactor ??
      NSScreen.main?.backingScaleFactor ??
      2.0
    view.frame = contentView.bounds
    view.autoresizingMask = [.width, .height]
    view.metalLayer.frame = view.bounds
    view.metalLayer.contentsScale = scale
    view.metalLayer.drawableSize = CGSize(
      width: max(16.0, CGFloat(width)),
      height: max(16.0, CGFloat(height))
    )
  }

  private func runOnMain<T>(_ body: () -> T) -> T {
    if Thread.isMainThread {
      return body()
    }
    return DispatchQueue.main.sync(execute: body)
  }
}

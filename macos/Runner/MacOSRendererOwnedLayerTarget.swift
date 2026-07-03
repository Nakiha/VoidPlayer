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
    metalLayer.presentsWithTransaction = false
    if #available(macOS 10.13, *) {
      metalLayer.displaySyncEnabled = true
      metalLayer.allowsNextDrawableTimeout = true
    }
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

private final class MacOSRendererOwnedCompositeCallbackContext {
  let target: MacOSRendererOwnedLayerTarget
  let drawable: CAMetalDrawable
  let textureObject: AnyObject
  let completion: MacOSRendererOwnedCompositeCompletion

  init(
    target: MacOSRendererOwnedLayerTarget,
    drawable: CAMetalDrawable,
    textureObject: AnyObject,
    completion: @escaping MacOSRendererOwnedCompositeCompletion
  ) {
    self.target = target
    self.drawable = drawable
    self.textureObject = textureObject
    self.completion = completion
  }
}

private func macOSRendererOwnedCompositeCompleted(
  userData: UnsafeMutableRawPointer?,
  result: Int32,
  frameInfo: UnsafePointer<VPMacOSNativeFrameInfo>?,
  error: UnsafePointer<CChar>?
) {
  guard let userData else { return }
  let context = Unmanaged<MacOSRendererOwnedCompositeCallbackContext>
    .fromOpaque(userData)
    .takeRetainedValue()
  let info = frameInfo.map { MacOSNativeFrameInfo(native: $0.pointee) }
  let message = error.map { String(cString: $0) } ?? ""
  context.target.completeRetainedComposite(
    drawable: context.drawable,
    textureObject: context.textureObject,
    result: result,
    frameInfo: info,
    error: message,
    completion: context.completion
  )
}

final class MacOSRendererOwnedLayerTarget: MacOSRendererOwnedPresentationTarget {
  private static let maxPendingDrawableLeases = 2

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
  private var retainedCompositeInFlightCount = 0
  private var retainedCompositeSubmitCount = 0
  private var retainedCompositePresentCount = 0
  private var retainedCompositeCompletionSuccessCount = 0
  private var retainedCompositeCompletionFailureCount = 0
  private var retainedCompositeFailureCount = 0
  private var drawableAcquireFailureCount = 0
  private var drawableSizeUpdateCount = 0
  private var drawableAcquireCount = 0
  private var drawableAcquireP95 = MacOSDurationWindow()
  private var lastRetainedCompositeCoalescedReason = "none"

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
    let view = self.view
    if Thread.isMainThread {
      view.removeFromSuperview()
    } else {
      DispatchQueue.main.async {
        view.removeFromSuperview()
      }
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

  func submitFromNativePlayer(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int
  ) throws {
    let token = try installNextDrawable(player: player, maxTrackSlots: maxTrackSlots)
    do {
      try player.submitRendererOwnedFrameRefresh()
    } catch {
      discardDrawable(address: token.pixelBufferAddress)
      uploadFailureCount += 1
      throw error
    }
  }

  func submitRetainedCompositeFromNativePlayer(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    completion: @escaping MacOSRendererOwnedCompositeCompletion
  ) throws {
    guard presentationTarget.isAvailable() else {
      throw MacOSNativePlayerError.failed(
        "renderer-owned Metal presentation backend is unavailable"
      )
    }
    lock.lock()
    let ready = viewportRectReady
    let currentViewportLeft = viewportLeft
    let currentViewportTop = viewportTop
    let currentViewportRight = viewportRight
    let currentViewportBottom = viewportBottom
    let currentInFlight = retainedCompositeInFlightCount
    lock.unlock()
    guard ready else {
      recordRetainedCompositeCoalesced(reason: "viewport-not-ready")
      throw MacOSNativePlayerError.transientFrameUnavailable(
        "renderer-owned Metal viewport is not ready"
      )
    }
    guard currentInFlight < Self.maxPendingDrawableLeases else {
      recordRetainedCompositeCoalesced(reason: "drawable-queue-busy")
      throw MacOSNativePlayerError.transientFrameUnavailable(
        "renderer-owned Metal drawable queue is busy"
      )
    }
    guard let drawable = nextDrawableOnMain() else {
      lock.lock()
      drawableAcquireFailureCount += 1
      lastRetainedCompositeCoalescedReason = "drawable-unavailable"
      lock.unlock()
      throw MacOSNativePlayerError.transientFrameUnavailable(
        "renderer-owned Metal drawable is unavailable"
      )
    }
    let texture = drawable.texture
    let textureObject = texture as AnyObject
    let context = MacOSRendererOwnedCompositeCallbackContext(
      target: self,
      drawable: drawable,
      textureObject: textureObject,
      completion: completion
    )
    let contextPointer = Unmanaged.passRetained(context).toOpaque()
    lock.lock()
    retainedCompositeInFlightCount += 1
    retainedCompositeSubmitCount += 1
    lock.unlock()
    do {
      try player.submitRetainedCompositeToMetalDrawable(
        texture: texture,
        width: texture.width,
        height: texture.height,
        maxTrackSlots: maxTrackSlots,
        viewportLeft: currentViewportLeft,
        viewportTop: currentViewportTop,
        viewportRight: currentViewportRight,
        viewportBottom: currentViewportBottom,
        completion: macOSRendererOwnedCompositeCompleted,
        userData: contextPointer
      )
    } catch {
      Unmanaged<MacOSRendererOwnedCompositeCallbackContext>
        .fromOpaque(contextPointer)
        .release()
      lock.lock()
      retainedCompositeInFlightCount = max(0, retainedCompositeInFlightCount - 1)
      retainedCompositeFailureCount += 1
      uploadFailureCount += 1
      lastRetainedCompositeCoalescedReason = "submit-error"
      lock.unlock()
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
      "rendererOwnedLayerInFlightDrawableCount": retainedCompositeInFlightCount,
      "rendererOwnedLayerUploadCount": uploadCount,
      "rendererOwnedLayerUploadFailureCount": uploadFailureCount,
      "rendererOwnedLayerTargetGeneration": targetGeneration,
      "rendererOwnedLayerTargetReinstallCount": 0,
      "rendererOwnedLayerDrawableSizeUpdateCount": drawableSizeUpdateCount,
      "rendererOwnedLayerDrawableAcquireCount": drawableAcquireCount,
      "rendererOwnedLayerDrawableAcquireFailureCount": drawableAcquireFailureCount,
      "rendererOwnedLayerDrawableAcquireP95Ms": drawableAcquireP95.p95Ms(),
      "rendererOwnedLayerDrawableAcquireP95MsX1000":
        Int(drawableAcquireP95.p95Ms() * 1000.0),
      "rendererOwnedLayerRetainedCompositeSubmitCount": retainedCompositeSubmitCount,
      "rendererOwnedLayerRetainedCompositePresentCount": retainedCompositePresentCount,
      "rendererOwnedLayerRetainedCompositeCompletionSuccessCount":
        retainedCompositeCompletionSuccessCount,
      "rendererOwnedLayerRetainedCompositeCompletionFailureCount":
        retainedCompositeCompletionFailureCount,
      "rendererOwnedLayerRetainedCompositePresentAfterCompletion":
        retainedCompositePresentCount <= retainedCompositeCompletionSuccessCount,
      "rendererOwnedLayerRetainedCompositeFailureCount": retainedCompositeFailureCount,
      "rendererOwnedLayerLastCoalescedReason": lastRetainedCompositeCoalescedReason,
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
      drawableSizeUpdateCount += 1
    }
    lock.unlock()
    guard changed else {
      updateFullSurfaceOnMain()
      return
    }
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
  ) -> MacOSNativeFramePublishOutcome {
    let nativeUploadCount = player.rendererOwnedPresentationUploadCount()
    lock.lock()
    if nativeUploadCount <= max(lastPublishedNativeUploadCount, lastIgnoredNativeUploadCount) {
      lock.unlock()
      return .notReady
    }
    lock.unlock()
    guard presentDrawable(address: frameInfo?.targetPixelBufferAddress ?? 0) else {
      lock.lock()
      lastIgnoredNativeUploadCount = max(lastIgnoredNativeUploadCount, nativeUploadCount)
      lock.unlock()
      return .notReady
    }
    lock.lock()
    lastPublishedNativeUploadCount = nativeUploadCount
    uploadCount += 1
    lock.unlock()
    return .published
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
    lock.lock()
    let pendingCount = pendingDrawables.count
    lock.unlock()
    guard pendingCount < Self.maxPendingDrawableLeases else {
      throw MacOSNativePlayerError.transientFrameUnavailable(
        "renderer-owned Metal drawable queue is busy"
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

  private func presentCompletedRetainedDrawable(
    drawable: CAMetalDrawable,
    textureObject: AnyObject
  ) {
    lock.lock()
    retainedCompositeCompletionSuccessCount += 1
    retainedCompositePresentCount += 1
    uploadCount += 1
    if !firstPresented {
      firstPresentViewportLeft = viewportLeft
      firstPresentViewportTop = viewportTop
      firstPresentViewportRight = viewportRight
      firstPresentViewportBottom = viewportBottom
      firstPresented = true
    }
    lock.unlock()
    DispatchQueue.main.async {
      _ = textureObject
      self.view.isHidden = false
      drawable.present()
    }
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
    let startNs = DispatchTime.now().uptimeNanoseconds
    let drawable = view.metalLayer.nextDrawable()
    let elapsedNs = DispatchTime.now().uptimeNanoseconds - startNs
    lock.lock()
    drawableAcquireCount += 1
    drawableAcquireP95.record(elapsedNs)
    if drawable != nil {
      lastRetainedCompositeCoalescedReason = "none"
    }
    lock.unlock()
    return drawable
  }

  private func recordRetainedCompositeCoalesced(reason: String) {
    lock.lock()
    lastRetainedCompositeCoalescedReason = reason
    lock.unlock()
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

  fileprivate func completeRetainedComposite(
    drawable: CAMetalDrawable,
    textureObject: AnyObject,
    result: Int32,
    frameInfo: MacOSNativeFrameInfo?,
    error: String,
    completion: @escaping MacOSRendererOwnedCompositeCompletion
  ) {
    lock.lock()
    retainedCompositeInFlightCount = max(0, retainedCompositeInFlightCount - 1)
    if result != 0 {
      retainedCompositeCompletionFailureCount += 1
      retainedCompositeFailureCount += 1
      uploadFailureCount += 1
    }
    lock.unlock()
    if result == 0 {
      presentCompletedRetainedDrawable(
        drawable: drawable,
        textureObject: textureObject
      )
      completion(.presented(frameInfo))
    } else {
      _ = drawable
      _ = textureObject
      completion(.failed(error.isEmpty
        ? "renderer-owned retained composite failed"
        : error))
    }
  }

  private func runOnMain<T>(_ body: () -> T) -> T {
    if Thread.isMainThread {
      return body()
    }
    return DispatchQueue.main.sync(execute: body)
  }
}

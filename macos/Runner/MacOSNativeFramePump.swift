import Foundation

final class MacOSNativeFrameCallbackContext {
  private let lock = NSLock()
  private weak var bridge: MacOSVideoRendererBridge?
  private var alive = true
  private var generation: UInt64 = 1

  init(bridge: MacOSVideoRendererBridge) {
    self.bridge = bridge
  }

  func invalidate() {
    lock.lock()
    alive = false
    generation &+= 1
    lock.unlock()
  }

  func isCurrent(_ expectedGeneration: UInt64) -> Bool {
    lock.lock()
    let current = alive && generation == expectedGeneration
    lock.unlock()
    return current
  }

  func scheduleFrameAvailable() {
    lock.lock()
    let currentBridge = alive ? bridge : nil
    let currentGeneration = generation
    lock.unlock()
    currentBridge?.scheduleNativeFrameCopyFromCallback(
      callbackGeneration: currentGeneration,
      callbackContext: self
    )
  }
}

func macOSNativeFrameAvailable(_ userData: UnsafeMutableRawPointer?) {
  guard let userData else { return }
  let context = Unmanaged<MacOSNativeFrameCallbackContext>
    .fromOpaque(userData)
    .takeUnretainedValue()
  context.scheduleFrameAvailable()
}

final class MacOSNativeFramePump {
  private var callbackRegistered = false
  private(set) var targetInstalled = false
  private var callbackContext: MacOSNativeFrameCallbackContext?

  func setTargetInstalled(_ installed: Bool) {
    targetInstalled = installed
  }

  func ensure(
    player: MacOSNativePlayerSession,
    texture: MacOSFlutterTextureBridge?,
    maxTrackSlots: Int,
    userData: UnsafeMutableRawPointer,
    presentationState: MacOSFramePresentationState
  ) -> Bool {
    if !targetInstalled, let texture {
      targetInstalled = texture.installNativePresentationTarget(
        player,
        maxTrackSlots: maxTrackSlots
      )
    }
    if !callbackRegistered {
      let bridge = Unmanaged<MacOSVideoRendererBridge>
        .fromOpaque(userData)
        .takeUnretainedValue()
      let context = MacOSNativeFrameCallbackContext(bridge: bridge)
      callbackContext = context
      player.setFrameAvailableCallback(
        macOSNativeFrameAvailable,
        userData: Unmanaged.passUnretained(context).toOpaque()
      )
      callbackRegistered = true
    }
    guard targetInstalled else {
      presentationState.recordError()
      NSLog("VoidPlayer macOS renderer-owned Metal presentation target unavailable")
      return false
    }
    return true
  }

  func start(
    player: MacOSNativePlayerSession,
    texture: MacOSFlutterTextureBridge?,
    maxTrackSlots: Int,
    userData: UnsafeMutableRawPointer,
    presentationState: MacOSFramePresentationState
  ) -> Bool {
    stop(player: player)
    presentationState.resetFrameCounters()
    player.resetRendererOwnedPresentationStats()
    texture?.resetNativeUploadBaseline()
    targetInstalled = false
    if !ensure(
      player: player,
      texture: texture,
      maxTrackSlots: maxTrackSlots,
      userData: userData,
      presentationState: presentationState
    ) {
      player.pause()
      stop(player: player)
      return false
    }
    return true
  }

  func stop(player: MacOSNativePlayerSession?, clearPresentationTarget: Bool = false) {
    callbackContext?.invalidate()
    if callbackRegistered {
      player?.setFrameAvailableCallback(nil, userData: nil)
      callbackRegistered = false
    }
    callbackContext = nil
    if clearPresentationTarget {
      player?.clearMetalPresentationTarget()
      targetInstalled = false
    }
  }
}

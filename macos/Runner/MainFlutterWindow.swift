import Cocoa
import CoreVideo
import FlutterMacOS

private struct MacOSNativeFrameInfo {
  let width: Int
  let height: Int
  let durationUs: Int
  let ptsUs: Int
  let dtsUs: Int
}

private struct MacOSNativeTrackMetadata {
  let fileId: Int
  let slot: Int
  let width: Int
  let height: Int
  let durationUs: Int
}

private enum MacOSNativePlayerError: Error, CustomStringConvertible {
  case failed(String)
  case invalidPayload
  case transientFrameUnavailable(String)

  var description: String {
    switch self {
    case .failed(let message):
      return message
    case .invalidPayload:
      return "native frame had invalid dimensions or pixel data"
    case .transientFrameUnavailable(let message):
      return message
    }
  }

  var isTransientFrameUnavailable: Bool {
    switch self {
    case .failed(let message):
      return message == "no decoded frame is ready" ||
        message == "not all present decision frames are ready" ||
        message == "timed out waiting for a decoded frame"
    case .invalidPayload:
      return false
    case .transientFrameUnavailable:
      return true
    }
  }
}

private final class MacOSNativePlayerSession {
  private let handle: OpaquePointer

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
    maxTrackSlots: Int
  ) -> Bool {
    VPMacOSNativePlayerSetMetalPresentationTarget(
      handle,
      backend,
      UnsafeMutableRawPointer(Unmanaged.passUnretained(pixelBuffer).toOpaque()),
      Int32(width),
      Int32(height),
      Int32(max(1, min(4, maxTrackSlots)))
    ) == 0
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
      dtsUs: Int(info.dts_us)
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

  func play() {
    VPMacOSNativePlayerPlay(handle)
  }

  func pause() {
    VPMacOSNativePlayerPause(handle)
  }

  func setSpeed(_ speed: Double) {
    VPMacOSNativePlayerSetSpeed(handle, speed)
  }

  func setLoopRange(enabled: Bool, startUs: Int, endUs: Int) {
    VPMacOSNativePlayerSetLoopRange(
      handle,
      enabled ? 1 : 0,
      Int64(startUs),
      Int64(endUs)
    )
  }

  func setAudibleTrack(_ fileId: Int) {
    VPMacOSNativePlayerSetAudibleTrack(handle, Int32(fileId))
  }

  func setTrackOffset(fileId: Int, offsetUs: Int) {
    VPMacOSNativePlayerSetTrackOffset(handle, Int32(fileId), Int64(offsetUs))
  }

  func trackOffsetUs(fileId: Int) -> Int {
    Int(VPMacOSNativePlayerTrackOffsetUs(handle, Int32(fileId)))
  }

  func applyLayout(
    mode: Int,
    splitPos: Double,
    zoomRatio: Double,
    viewOffsetX: Double,
    viewOffsetY: Double,
    pixelSizeMode: Int,
    order: [Int]
  ) {
    let paddedOrder = (order + [0, 0, 0, 0]).prefix(4).map { Int32($0) }
    var state = VPMacOSNativeLayoutState()
    state.mode = Int32(mode)
    state.split_pos = Float(splitPos)
    state.zoom_ratio = Float(zoomRatio)
    state.view_offset_x = Float(viewOffsetX)
    state.view_offset_y = Float(viewOffsetY)
    state.pixel_size_mode = Int32(pixelSizeMode)
    state.order = (paddedOrder[0], paddedOrder[1], paddedOrder[2], paddedOrder[3])
    VPMacOSNativePlayerApplyLayout(handle, &state)
  }

  func layoutSnapshotMap() -> [String: Any]? {
    var state = VPMacOSNativeLayoutState()
    guard VPMacOSNativePlayerCopyLayout(handle, &state) == 0 else {
      return nil
    }
    return [
      "mode": Int(state.mode),
      "splitPos": Double(state.split_pos),
      "zoomRatio": Double(state.zoom_ratio),
      "viewOffsetX": Double(state.view_offset_x),
      "viewOffsetY": Double(state.view_offset_y),
      "pixelSizeMode": Int(state.pixel_size_mode),
      "order": [
        Int(state.order.0),
        Int(state.order.1),
        Int(state.order.2),
        Int(state.order.3),
      ],
    ]
  }

  func seek(_ ptsUs: Int) {
    VPMacOSNativePlayerSeek(handle, Int64(ptsUs))
  }

  func stepForward() throws {
    var error = [CChar](repeating: 0, count: 1024)
    let ret = VPMacOSNativePlayerStepForward(handle, &error, error.count)
    if ret != 0 {
      let message = String(cString: error)
      throw MacOSNativePlayerError.failed(
        message.isEmpty ? "macOS native player stepForward failed with code \(ret)" : message
      )
    }
  }

  func stepBackward() throws {
    var error = [CChar](repeating: 0, count: 1024)
    let ret = VPMacOSNativePlayerStepBackward(handle, &error, error.count)
    if ret != 0 {
      let message = String(cString: error)
      throw MacOSNativePlayerError.failed(
        message.isEmpty ? "macOS native player stepBackward failed with code \(ret)" : message
      )
    }
  }

  func currentPtsUs() -> Int {
    Int(VPMacOSNativePlayerCurrentPtsUs(handle))
  }

  func durationUs() -> Int {
    Int(VPMacOSNativePlayerDurationUs(handle))
  }

  func width() -> Int {
    Int(VPMacOSNativePlayerWidth(handle))
  }

  func height() -> Int {
    Int(VPMacOSNativePlayerHeight(handle))
  }

  func isPlaying() -> Bool {
    VPMacOSNativePlayerIsPlaying(handle) != 0
  }

  func hasAudio() -> Bool {
    VPMacOSNativePlayerHasAudio(handle) != 0
  }

  func audioSampleRate() -> Int {
    Int(VPMacOSNativePlayerAudioSampleRate(handle))
  }

  func audioChannels() -> Int {
    Int(VPMacOSNativePlayerAudioChannels(handle))
  }

  func activeAudioTrack() -> Int {
    Int(VPMacOSNativePlayerActiveAudioTrack(handle))
  }

  func hardwareDecodeActive() -> Bool {
    VPMacOSNativePlayerHardwareDecodeActive(handle) != 0
  }

  func hardwareDecodeDownloadsToCpu() -> Bool {
    VPMacOSNativePlayerHardwareDecodeDownloadsToCpu(handle) != 0
  }

  func decodeModeName() -> String {
    String(cString: VPMacOSNativePlayerDecodeModeName(handle))
  }

  func decoderName() -> String {
    String(cString: VPMacOSNativePlayerDecoderName(handle))
  }

  func presentationSchedulerStats() -> [String: Any] {
    var stats = VPMacOSNativePresentationSchedulerStats()
    guard VPMacOSNativePlayerCopyPresentationSchedulerStats(handle, &stats) == 0 else {
      return [
        "tickCount": 0,
        "presentableTickCount": 0,
        "frameNotificationCount": 0,
        "lastSelectedPtsUs": -1,
        "lastPresentFrameCount": 0,
        "cachedPresentDecisionAvailable": false,
        "deadlineSleepCount": 0,
        "lastDeadlineSleepUs": 0,
      ]
    }
    let maxInt64 = UInt64(Int64.max)
    return [
      "tickCount": Int64(min(UInt64(stats.tick_count), maxInt64)),
      "presentableTickCount": Int64(min(UInt64(stats.presentable_tick_count), maxInt64)),
      "frameNotificationCount": Int64(min(UInt64(stats.frame_notification_count), maxInt64)),
      "lastSelectedPtsUs": Int64(stats.last_selected_pts_us),
      "lastPresentFrameCount": Int(stats.last_present_frame_count),
      "cachedPresentDecisionAvailable": stats.cached_present_decision_available != 0,
      "deadlineSleepCount": Int64(min(UInt64(stats.deadline_sleep_count), maxInt64)),
      "lastDeadlineSleepUs": Int64(stats.last_deadline_sleep_us),
    ]
  }

  func performanceStats() -> [String: Any] {
    var stats = VPMacOSNativePlayerPerfStats()
    guard VPMacOSNativePlayerCopyPerfStats(handle, &stats) == 0 else {
      return [
        "decodeFrameCount": 0,
        "decodeDroppedCount": 0,
        "decodeElapsedMs": 0,
        "decodeFps": 0.0,
        "decodeFpsX1000": 0,
        "decodeAvgMs": 0.0,
        "decodeMaxMs": 0.0,
        "rendererOwnedUploadCount": 0,
        "rendererOwnedUploadFailureCount": 0,
        "rendererOwnedUploadElapsedMs": 0,
        "rendererOwnedUploadFps": 0.0,
        "rendererOwnedUploadFpsX1000": 0,
      ]
    }
    let maxInt64 = UInt64(Int64.max)
    return [
      "decodeFrameCount": Int64(min(UInt64(stats.decode_frame_count), maxInt64)),
      "decodeDroppedCount": Int64(min(UInt64(stats.decode_dropped_count), maxInt64)),
      "decodeElapsedMs": Int64(stats.decode_elapsed_ms),
      "decodeFps": stats.decode_fps,
      "decodeFpsX1000": Int64(max(0.0, stats.decode_fps * 1000.0)),
      "decodeAvgMs": stats.decode_avg_ms,
      "decodeMaxMs": stats.decode_max_ms,
      "rendererOwnedUploadCount": Int64(
        min(UInt64(stats.renderer_owned_upload_count), maxInt64)
      ),
      "rendererOwnedUploadFailureCount": Int64(
        min(UInt64(stats.renderer_owned_upload_failure_count), maxInt64)
      ),
      "rendererOwnedUploadElapsedMs": Int64(stats.renderer_owned_upload_elapsed_ms),
      "rendererOwnedUploadFps": stats.renderer_owned_upload_fps,
      "rendererOwnedUploadFpsX1000": Int64(
        max(0.0, stats.renderer_owned_upload_fps * 1000.0)
      ),
    ]
  }

  func copyPresentationIntoBGRA(
    _ dst: UnsafeMutablePointer<UInt8>,
    dstSize: Int,
    width: Int,
    height: Int,
    strideBytes: Int,
    waitTimeoutMs: Int = 0
  ) throws -> MacOSNativeFrameInfo {
    let deadline = Date().addingTimeInterval(Double(waitTimeoutMs) / 1000.0)
    var lastError = ""

    repeat {
      var frameInfo = VPMacOSNativeFrameInfo()
      var error = [CChar](repeating: 0, count: 1024)
      let ret = VPMacOSNativePlayerCopyPresentationBGRAInto(
        handle,
        dst,
        dstSize,
        Int32(width),
        Int32(height),
        Int32(strideBytes),
        &frameInfo,
        &error,
        error.count
      )
      if ret == 0 {
        guard frameInfo.width > 0, frameInfo.height > 0 else {
          throw MacOSNativePlayerError.invalidPayload
        }
        return MacOSNativeFrameInfo(
          width: Int(frameInfo.width),
          height: Int(frameInfo.height),
          durationUs: Int(frameInfo.duration_us),
          ptsUs: Int(frameInfo.pts_us),
          dtsUs: Int(frameInfo.dts_us)
        )
      }
      let message = String(cString: error)
      lastError = message.isEmpty
        ? "copyPresentationIntoBGRA failed with code \(ret)"
        : message
      if Date() < deadline {
        Thread.sleep(forTimeInterval: 0.01)
      }
    } while Date() < deadline

    throw MacOSNativePlayerError.failed(
      lastError.isEmpty ? "timed out waiting for a presentable frame" : lastError
    )
  }

  func copyCurrentFrameToMetalPixelBuffer(
    backend: OpaquePointer,
    pixelBuffer: CVPixelBuffer,
    width: Int,
    height: Int,
    maxTrackSlots: Int,
    waitTimeoutMs: Int = 0
  ) throws -> MacOSNativeFrameInfo? {
    let deadline = Date().addingTimeInterval(Double(waitTimeoutMs) / 1000.0)
    var lastError = ""

    repeat {
      var frameInfo = VPMacOSNativeFrameInfo()
      var error = [CChar](repeating: 0, count: 1024)
      let ret = VPMacOSMetalPresentationBackendCopyCurrentFrameWithLayout(
        backend,
        handle,
        UnsafeMutableRawPointer(Unmanaged.passUnretained(pixelBuffer).toOpaque()),
        Int32(width),
        Int32(height),
        Int32(max(1, min(4, maxTrackSlots))),
        Int32(waitTimeoutMs),
        &frameInfo,
        &error,
        error.count
      )
      if ret == 0 {
        guard frameInfo.width > 0, frameInfo.height > 0 else {
          return nil
        }
        return MacOSNativeFrameInfo(
          width: Int(frameInfo.width),
          height: Int(frameInfo.height),
          durationUs: Int(frameInfo.duration_us),
          ptsUs: Int(frameInfo.pts_us),
          dtsUs: Int(frameInfo.dts_us)
        )
      }
      let message = String(cString: error)
      lastError = message.isEmpty
        ? "copyCurrentFrameToMetalPixelBuffer failed with code \(ret)"
        : message
      if Date() < deadline {
        Thread.sleep(forTimeInterval: 0.01)
      }
    } while Date() < deadline

    if lastError == "no presentable frame is ready" ||
        lastError == "not all present decision frames are ready" {
      return nil
    }
    throw MacOSNativePlayerError.failed(
      lastError.isEmpty ? "timed out waiting for a decoded frame" : lastError
    )
  }
}

private func macOSNativeFrameAvailable(_ userData: UnsafeMutableRawPointer?) {
  guard let userData else { return }
  let renderer = Unmanaged<MacOSVideoRendererStub>.fromOpaque(userData).takeUnretainedValue()
  renderer.scheduleNativeFrameCopyFromCallback()
}

class MainFlutterWindow: NSWindow {
  override func close() {
    MacOSVideoRendererStub.destroyActivePlayerForWindowClose()
    super.close()
  }

  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    MacOSVideoRendererStub.register(with: flutterViewController.engine)
    RegisterGeneratedPlugins(registry: flutterViewController)

    super.awakeFromNib()
  }
}

private final class MacOSVideoRendererStub: NSObject, FlutterStreamHandler {
  private static let channelName = "video_renderer"
  private static let eventsChannelName = "video_renderer/events"
  private static let syntheticDurationUs = 10_000_000
  private static let metalUploadDisabledForTest =
    ProcessInfo.processInfo.arguments.contains("--macos-disable-metal-upload")
  private static weak var activeInstance: MacOSVideoRendererStub?

  private let textureRegistry: FlutterTextureRegistry
  private let playbackQueue = DispatchQueue(label: "dev.nakiha.voidplayer.macos.native-playback")
  private var methodChannel: FlutterMethodChannel?
  private var eventChannel: FlutterEventChannel?
  private var texture: MacOSFlutterTextureBridge?
  private var textureId: Int64?
  private var eventSink: FlutterEventSink?
  private var nativeEventListenCount = 0
  private var nativeEventEmitCount = 0
  private var nativeEventDropNoSinkCount = 0
  private var nativeEventSequence = 0
  private var tracks: [[String: Any]] = []
  private var layout: [String: Any] = MacOSVideoRendererStub.defaultLayout()
  private var currentPtsUs = 0
  private var currentDurationUs = 0
  private var lastPresentedPtsUs: Int?
  private var lastPresentedDtsUs: Int?
  private var lastPresentedDurationUs: Int?
  private var isPlaying = false
  private var backendName = "synthetic-texture"
  private var nativePlayer: MacOSNativePlayerSession?
  private var playbackSpeed = 1.0
  private var nativeFrameCallbackRegistered = false
  private var playbackGeneration = 0
  private var nativeFrameCallbackCount = 0
  private var nativeFrameCopyCount = 0
  private var nativeFrameRendererOwnedPresentCount = 0
  private var nativeFrameSwiftCopyCount = 0
  private var nativeFrameCopyMissCount = 0
  private var nativeFrameCopyErrorCount = 0
  private var nativeFrameCopyCoalescedCount = 0
  private var nativeFrameCopyInFlight = false
  private var nativePresentationTargetInstalled = false
  private var nativeFrameCopyFirstHostNs: UInt64?
  private var nativeFrameCopyLastHostNs: UInt64?
  private let presentedPtsTraceCapacity = 240
  private var presentedPtsTrace: [Int] = []
  private var presentedPtsSampleCount = 0
  private var presentedPtsDistinctCount = 0
  private var presentedPtsFirstUs: Int?
  private var presentedPtsLastStepUs = 0
  private var presentedPtsMonotonicViolationCount = 0

  init(textureRegistry: FlutterTextureRegistry) {
    self.textureRegistry = textureRegistry
    super.init()
  }

  static func register(with engine: FlutterEngine) {
    configureNativeEnvironment()
    let stub = MacOSVideoRendererStub(textureRegistry: engine)
    activeInstance = stub
    let messenger = engine.binaryMessenger
    let channel = FlutterMethodChannel(name: channelName, binaryMessenger: messenger)
    channel.setMethodCallHandler(stub.handle)
    stub.methodChannel = channel

    let events = FlutterEventChannel(name: eventsChannelName, binaryMessenger: messenger)
    events.setStreamHandler(stub)
    stub.eventChannel = events
  }

  private static func configureNativeEnvironment() {
    if metalUploadDisabledForTest {
      setenv("VOIDPLAYER_FORCE_VIDEOTOOLBOX_HWDOWNLOAD", "1", 1)
    }
  }

  static func destroyActivePlayerForWindowClose() {
    activeInstance?.destroyPlayerForWindowClose()
  }

  private func configureNativeLogging(arguments: Any?) {
    guard let args = arguments as? [String: Any],
          let logsDir = args["logsDir"] as? String,
          !logsDir.isEmpty else {
      return
    }
    logsDir.withCString { logsDirPointer in
      VPMacOSInstallCrashHandler(logsDirPointer)
    }
  }

  private func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "initLogging":
      configureNativeLogging(arguments: call.arguments)
      result(nil)
    case "setViewportBackgroundColor":
      result(nil)
    case "setTrackOffset":
      let fileId = intArg(call.arguments, "fileId") ?? -1
      let offsetUs = intArg(call.arguments, "offsetUs") ?? 0
      nativePlayer?.setTrackOffset(fileId: fileId, offsetUs: offsetUs)
      result(nil)
    case "setLoopRange":
      nativePlayer?.setLoopRange(
        enabled: boolArg(call.arguments, "enabled") ?? false,
        startUs: intArg(call.arguments, "startUs") ?? 0,
        endUs: intArg(call.arguments, "endUs") ?? 0
      )
      result(nil)
    case "setAudibleTrack":
      let fileId = intArg(call.arguments, "fileId") ?? -1
      nativePlayer?.setAudibleTrack(fileId)
      result(nil)
    case "setSpeed":
      playbackSpeed = max(0.01, doubleArg(call.arguments, "speed") ?? 1.0)
      nativePlayer?.setSpeed(playbackSpeed)
      result(nil)
    case "createPlayer":
      result(createPlayer(arguments: call.arguments))
    case "destroyPlayer":
      destroyPlayer()
      result(nil)
    case "addTrack":
      result(addTrack(arguments: call.arguments))
    case "removeTrack":
      removeTrack(arguments: call.arguments)
      result(nil)
    case "resize":
      resize(arguments: call.arguments)
      result(nil)
    case "play":
      isPlaying = textureId != nil
      nativePlayer?.play()
      startNativeFramePump()
      result(nil)
    case "pause":
      isPlaying = false
      nativePlayer?.pause()
      stopNativeFramePump()
      result(nil)
    case "seek":
      let targetPtsUs = intArg(call.arguments, "ptsUs") ?? 0
      let requestId = intArg(call.arguments, "requestId")
      let resumeAfterSeek = nativePlayer?.isPlaying() ?? isPlaying
      if let error = seekAndRefresh(
        targetPtsUs: targetPtsUs,
        requestId: requestId,
        resumeAfterSeek: resumeAfterSeek
      ) {
        result(error)
        return
      }
      result(nil)
    case "stepForward":
      if let error = stepAndRefresh(forward: true) {
        result(error)
        return
      }
      result(nil)
    case "stepBackward":
      if let error = stepAndRefresh(forward: false) {
        result(error)
        return
      }
      result(nil)
    case "currentPts":
      currentPtsUs = nativePlayer?.currentPtsUs() ?? currentPtsUs
      result(currentPtsUs)
    case "duration":
      result(tracks.isEmpty ? 0 : currentDurationUs)
    case "currentPresentedFrame":
      result(
        textureId == nil
          ? nil
          : [
              "ptsUs": lastPresentedPtsUs ?? currentPtsUs,
              "dtsUs": lastPresentedDtsUs ?? lastPresentedPtsUs ?? currentPtsUs,
              "durationUs": lastPresentedDurationUs ?? 0,
            ]
      )
    case "isPlaying":
      result(nativePlayer?.isPlaying() ?? isPlaying)
    case "getLayout":
      result(layout)
    case "applyLayout":
      if let nextLayout = call.arguments as? [String: Any] {
        nativePlayer?.applyLayout(
          mode: intValue(nextLayout["mode"]) ?? 0,
          splitPos: doubleValue(nextLayout["splitPos"]) ?? 0.5,
          zoomRatio: doubleValue(nextLayout["zoomRatio"]) ?? 1.0,
          viewOffsetX: doubleValue(nextLayout["viewOffsetX"]) ?? 0.0,
          viewOffsetY: doubleValue(nextLayout["viewOffsetY"]) ?? 0.0,
          pixelSizeMode: intValue(nextLayout["pixelSizeMode"]) ?? 0,
          order: intListValue(nextLayout["order"])
        )
        layout = nativePlayer?.layoutSnapshotMap() ?? nextLayout
        refreshCurrentFrameAfterLayoutChange()
      }
      result(nil)
    case "getTracks":
      result(tracks)
    case "pickFiles":
      pickFiles(arguments: call.arguments, result: result)
    case "getDiagnostics":
      let textureStats = texture?.diagnostics()
      let textureDimensions = texture?.dimensions()
      let nativeLayoutSnapshot = nativePlayer?.layoutSnapshotMap()
      let schedulerStats = nativePlayer?.presentationSchedulerStats()
      let perfStats = nativePlayer?.performanceStats()
      let diagnostics: [String: Any] = [
        "platform": "macos",
        "backend": backendName,
        "presentationAdapter": String(cString: VPMacOSNativePresentationAdapterName()),
        "presentationAdapterKind": "software-fallback",
        "presentationScheduler": String(cString: VPMacOSNativePresentationSchedulerName()),
        "presentationBackend": nativePlayer?.rendererOwnedPresentationActive() == true
          ? "native-metal-cvpixelbuffer-target"
          : "swift-cvpixelbuffer-texture-pump",
        "rendererOwnedPresentationActive": nativePlayer?.rendererOwnedPresentationActive() ?? false,
        "hardwareDecodeProvider": String(cString: VPMacOSNativeHardwareDecodeProviderName()),
        "hardwareDecodeAvailable": VPMacOSNativeHardwareDecodeAvailable() != 0,
        "hardwareDecodeActive": nativePlayer?.hardwareDecodeActive() ?? false,
        "hardwareDecodeDownloadsToCpu": nativePlayer?.hardwareDecodeDownloadsToCpu() ?? false,
        "decodeMode": nativePlayer?.decodeModeName() ?? "none",
        "softwareFallbackActive": nativePlayer?.hardwareDecodeActive() != true,
        "available": nativePlayer != nil,
        "reason": nativePlayer == nil
          ? "Synthetic macOS texture bridge is active"
          : (nativePlayer?.rendererOwnedPresentationActive() == true
            ? "macOS shared native facade is active with renderer-owned Metal presentation"
            : "macOS shared native facade is active with transitional texture-pump presentation"),
        "textureId": textureId ?? -1,
        "textureWidth": textureDimensions?.width ?? 0,
        "textureHeight": textureDimensions?.height ?? 0,
        "trackCount": tracks.count,
        "audioAvailable": nativePlayer?.hasAudio() ?? false,
        "audioSampleRate": nativePlayer?.audioSampleRate() ?? 0,
        "audioChannels": nativePlayer?.audioChannels() ?? 0,
        "activeAudioTrack": nativePlayer?.activeAudioTrack() ?? -1,
        "primaryTrackOffsetUs": nativePlayer?.trackOffsetUs(fileId: 0) ?? 0,
        "secondaryTrackOffsetUs": nativePlayer?.trackOffsetUs(fileId: 1) ?? 0,
        "presentationSchedulerTickCount": schedulerStats?["tickCount"] ?? 0,
        "presentationSchedulerPresentableTickCount": schedulerStats?["presentableTickCount"] ?? 0,
        "presentationSchedulerFrameNotificationCount": schedulerStats?["frameNotificationCount"] ?? 0,
        "presentationSchedulerLastSelectedPtsUs": schedulerStats?["lastSelectedPtsUs"] ?? -1,
        "presentationSchedulerLastPresentFrameCount": schedulerStats?["lastPresentFrameCount"] ?? 0,
        "presentationSchedulerCachedDecisionAvailable": schedulerStats?["cachedPresentDecisionAvailable"] ?? false,
        "presentationSchedulerDeadlineSleepCount": schedulerStats?["deadlineSleepCount"] ?? 0,
        "presentationSchedulerLastDeadlineSleepUs": schedulerStats?["lastDeadlineSleepUs"] ?? 0,
        "nativeLayoutMode": nativeLayoutSnapshot?["mode"] ?? -1,
        "nativeLayoutZoomRatio": nativeLayoutSnapshot?["zoomRatio"] ?? 0.0,
        "nativeLayoutPixelSizeMode": nativeLayoutSnapshot?["pixelSizeMode"] ?? -1,
        "pixelBufferRebuildCount": textureStats?.rebuildCount ?? 0,
        "pixelBufferReuseCount": textureStats?.reuseCount ?? 0,
        "pixelBufferDirectCopyCount": textureStats?.directCopyCount ?? 0,
        "pixelBufferMetalUploadCount": textureStats?.metalUploadCount ?? 0,
        "pixelBufferMetalYuvUploadCount": textureStats?.metalYuvUploadCount ?? 0,
        "pixelBufferMetalCVPixelBufferUploadCount": textureStats?.metalCVPixelBufferUploadCount ?? 0,
        "pixelBufferMetalUploadFailureCount": textureStats?.metalUploadFailureCount ?? 0,
        "pixelBufferMetalUploadEnabled": textureStats?.metalUploadEnabled ?? false,
        "presentationUploadMode": textureStats?.presentationUploadMode ?? "unavailable",
        "presentationPackageUploadCount": textureStats?.presentPackageUploadCount ?? 0,
        "presentationPackageCopyUs": textureStats?.presentPackageCopyUs ?? 0,
        "presentationPackageGpuWaitUs": textureStats?.presentPackageGpuWaitUs ?? 0,
        "presentationPackageTotalUs": textureStats?.presentPackageTotalUs ?? 0,
        "presentationPackageStorage": textureStats?.presentPackageStorage ?? "unavailable",
        "metalAvailable": textureStats?.metalAvailable ?? false,
        "metalTextureCacheAvailable": textureStats?.metalTextureCacheAvailable ?? false,
        "metalTextureValid": textureStats?.metalTextureValid ?? false,
        "metalTextureCreationCount": textureStats?.metalTextureCreationCount ?? 0,
        "metalTextureFailureCount": textureStats?.metalTextureFailureCount ?? 0,
        "metalTextureLastError": textureStats?.metalTextureLastError ?? "",
        "nativeFrameCallbackCount": nativeFrameCallbackCount,
        "nativeFrameCopyCount": nativeFrameCopyCount,
        "nativeFrameRendererOwnedPresentCount": nativeFrameRendererOwnedPresentCount,
        "nativeFrameSwiftCopyCount": nativeFrameSwiftCopyCount,
        "nativeFrameCopyMissCount": nativeFrameCopyMissCount,
        "nativeFrameCopyErrorCount": nativeFrameCopyErrorCount,
        "nativeFrameCopyCoalescedCount": nativeFrameCopyCoalescedCount,
        "nativeEventListenCount": nativeEventListenCount,
        "nativeEventEmitCount": nativeEventEmitCount,
        "nativeEventDropNoSinkCount": nativeEventDropNoSinkCount,
        "nativePresentationTargetInstalled": nativePresentationTargetInstalled,
        "nativeRendererOwnedUploadCount": nativePlayer?.rendererOwnedPresentationUploadCount() ?? 0,
        "nativeRendererOwnedUploadFailureCount": nativePlayer?.rendererOwnedPresentationFailureCount() ?? 0,
        "nativeRendererOwnedUploadFps": perfStats?["rendererOwnedUploadFps"] ?? 0.0,
        "nativeRendererOwnedUploadFpsX1000": perfStats?["rendererOwnedUploadFpsX1000"] ?? 0,
        "nativeRendererOwnedUploadElapsedMs": perfStats?["rendererOwnedUploadElapsedMs"] ?? 0,
        "nativeDecodeFrameCount": perfStats?["decodeFrameCount"] ?? 0,
        "nativeDecodeDroppedCount": perfStats?["decodeDroppedCount"] ?? 0,
        "nativeDecodeElapsedMs": perfStats?["decodeElapsedMs"] ?? 0,
        "nativeDecodeFps": perfStats?["decodeFps"] ?? 0.0,
        "nativeDecodeFpsX1000": perfStats?["decodeFpsX1000"] ?? 0,
        "nativeDecodeAvgMs": perfStats?["decodeAvgMs"] ?? 0.0,
        "nativeDecodeMaxMs": perfStats?["decodeMaxMs"] ?? 0.0,
        "presentationFallbackReason": presentationFallbackReason(perfStats: perfStats),
        "nativeFrameCopyElapsedMs": nativeFrameCopyElapsedMs(),
        "nativeFrameCopyFps": nativeFrameCopyFps(),
        "nativeFrameCopyFpsX1000": Int(nativeFrameCopyFps() * 1000.0),
        "presentedFramePtsSampleCount": presentedPtsSampleCount,
        "presentedFramePtsDistinctCount": presentedPtsDistinctCount,
        "presentedFramePtsFirstUs": presentedPtsFirstUs ?? -1,
        "presentedFramePtsLastUs": lastPresentedPtsUs ?? -1,
        "presentedFrameDtsLastUs": lastPresentedDtsUs ?? -1,
        "presentedFrameDurationLastUs": lastPresentedDurationUs ?? 0,
        "presentedFramePtsAdvanceUs": presentedPtsAdvanceUs(),
        "presentedFramePtsLastStepUs": presentedPtsLastStepUs,
        "presentedFramePtsMonotonicViolationCount": presentedPtsMonotonicViolationCount,
        "presentedFramePtsTrace": presentedPtsTrace
          .suffix(32)
          .map { String($0) }
          .joined(separator: ","),
      ]
      result(diagnostics)
    case "captureViewport":
      result(captureViewport())
    default:
      result(FlutterMethodNotImplemented)
    }
  }

  private func createPlayer(arguments: Any?) -> Any {
    destroyPlayer()

    let paths = stringListArg(arguments, "videoPaths")
    let requestedWidth = max(16, intArg(arguments, "width") ?? 1920)
    let requestedHeight = max(16, intArg(arguments, "height") ?? 1080)
    let firstPath = paths.first ?? "macos-synthetic://color-bars"

    let nextTexture: MacOSFlutterTextureBridge
    let trackWidth: Int
    let trackHeight: Int
    let trackDurationUs: Int
    let trackFormatName: String
    let trackCodecName: String
    let trackCodecLongName: String
    let trackDecoderName: String
    var initialPresentedPtsUs = 0
    var initialPresentedDtsUs = 0
    if firstPath.hasPrefix("macos-synthetic://") {
      nextTexture = MacOSFlutterTextureBridge(
        width: requestedWidth,
        height: requestedHeight,
        metalUploadEnabled: !Self.metalUploadDisabledForTest
      )
      trackWidth = requestedWidth
      trackHeight = requestedHeight
      trackDurationUs = Self.syntheticDurationUs
      trackFormatName = "synthetic"
      trackCodecName = "macos_synthetic"
      trackCodecLongName = "macOS Synthetic FlutterTexture"
      trackDecoderName = "synthetic"
      backendName = "synthetic-texture"
      nativePlayer = nil
    } else {
      do {
        guard let session = MacOSNativePlayerSession() else {
          throw MacOSNativePlayerError.failed("failed to allocate macOS native player")
        }
        try session.open(path: firstPath)
        nextTexture = MacOSFlutterTextureBridge(
          nativeWidth: requestedWidth,
          nativeHeight: requestedHeight,
          metalUploadEnabled: !Self.metalUploadDisabledForTest
        )
        let firstFrame = try nextTexture.updateFromNativePlayer(
          session,
          maxTrackSlots: 1,
          waitTimeoutMs: 3_000
        )
        initialPresentedPtsUs = firstFrame.ptsUs
        initialPresentedDtsUs = normalizedDtsUs(firstFrame)
        trackWidth = session.width() > 0 ? session.width() : firstFrame.width
        trackHeight = session.height() > 0 ? session.height() : firstFrame.height
        trackDurationUs = session.durationUs() > 0 ? session.durationUs() : Self.syntheticDurationUs
        trackFormatName = "macos-native-player"
        trackCodecName = "ffmpeg"
        trackCodecLongName = "macOS shared native DecodeThread facade"
        trackDecoderName = session.decoderName()
        backendName = "macos-native-player"
        nativePlayer = session
      } catch {
        return FlutterError(
          code: "DECODE_FAILED",
          message: "Failed to open macOS native player",
          details: "\(error)"
        )
      }
    }

    let registeredTextureId = textureRegistry.register(nextTexture)

    texture = nextTexture
    textureId = registeredTextureId
    if backendName == "macos-native-player" {
      tracks = [
        trackMap(
          fileId: 0,
          slot: 0,
          path: firstPath,
          width: trackWidth,
          height: trackHeight,
          durationUs: trackDurationUs,
          formatName: trackFormatName,
          codecName: trackCodecName,
          codecLongName: trackCodecLongName,
          decoderName: trackDecoderName
        )
      ]
      if paths.count > 1 {
        for path in paths.dropFirst() {
          let fileId = (tracks.map { intValue($0["fileId"]) ?? 0 }.max() ?? -1) + 1
          do {
            guard let session = nativePlayer else {
              throw MacOSNativePlayerError.failed("macOS native player is unavailable")
            }
            let metadata = try session.addTrack(path: path, fileId: fileId)
            tracks.append(nativeTrackMap(path: path, metadata: metadata))
          } catch {
            destroyPlayer()
            return FlutterError(
              code: "DECODE_FAILED",
              message: "Failed to add macOS native track",
              details: "\(error)"
            )
          }
        }
      }
    } else {
      tracks = paths.enumerated().map { index, path in
        trackMap(
          fileId: index,
          slot: index,
          path: path,
          width: trackWidth,
          height: trackHeight,
          durationUs: trackDurationUs,
          formatName: trackFormatName,
          codecName: trackCodecName,
          codecLongName: trackCodecLongName,
          decoderName: trackDecoderName
        )
      }
    }
    currentDurationUs = tracks
      .map { intValue($0["durationUs"]) ?? trackDurationUs }
      .max() ?? trackDurationUs
    currentPtsUs = initialPresentedPtsUs
    lastPresentedPtsUs = initialPresentedPtsUs
    lastPresentedDtsUs = initialPresentedDtsUs
    lastPresentedDurationUs = trackDurationUs
    isPlaying = false
    markFrameAvailable()

    return [
      "textureId": registeredTextureId,
      "tracks": tracks,
    ]
  }

  private func nativeTrackMap(path: String, metadata: MacOSNativeTrackMetadata) -> [String: Any] {
    trackMap(
      fileId: metadata.fileId,
      slot: metadata.slot,
      path: path,
      width: metadata.width,
      height: metadata.height,
      durationUs: metadata.durationUs,
      formatName: "macos-native-player",
      codecName: "ffmpeg",
      codecLongName: "macOS shared native DecodeThread facade",
      decoderName: nativePlayer?.decoderName() ?? "decode_thread_software"
    )
  }

  private func destroyPlayer() {
    stopNativeFramePump()
    if let id = textureId {
      textureRegistry.unregisterTexture(id)
    }
    texture = nil
    textureId = nil
    tracks.removeAll()
    currentPtsUs = 0
    currentDurationUs = 0
    lastPresentedPtsUs = nil
    lastPresentedDtsUs = nil
    lastPresentedDurationUs = nil
    isPlaying = false
    backendName = "synthetic-texture"
    nativePlayer?.close()
    nativePlayer = nil
  }

  private func destroyPlayerForWindowClose() {
    if textureId == nil && nativePlayer == nil {
      return
    }
    NSLog("VoidPlayer macOS native player teardown before window close")
    destroyPlayer()
  }

  private func addTrack(arguments: Any?) -> Any {
    guard textureId != nil else {
      return FlutterError(
        code: "NO_PLAYER",
        message: "createPlayer must be called before addTrack",
        details: nil
      )
    }

    let fileId = (tracks.map { intValue($0["fileId"]) ?? 0 }.max() ?? -1) + 1
    let slot = tracks.count
    let path = stringArg(arguments, "path") ?? "macos-synthetic-\(fileId)"
    if backendName == "macos-native-player" {
      do {
        guard let session = nativePlayer else {
          throw MacOSNativePlayerError.failed("macOS native player is unavailable")
        }
        let metadata = try session.addTrack(path: path, fileId: fileId)
        let track = nativeTrackMap(path: path, metadata: metadata)
        tracks.append(track)
        currentDurationUs = max(currentDurationUs, metadata.durationUs)
        refreshCurrentFrameAfterLayoutChange()
        return track
      } catch {
        return FlutterError(
          code: "DECODE_FAILED",
          message: "Failed to add macOS native track",
          details: "\(error)"
        )
      }
    }

    let size = texture?.dimensions() ?? (width: 1920, height: 1080)
    let track = trackMap(
      fileId: fileId,
      slot: slot,
      path: path,
      width: size.width,
      height: size.height,
      durationUs: currentDurationUs,
      formatName: backendName,
      codecName: backendName == "macos-native-player" ? "ffmpeg" : "macos_synthetic",
      codecLongName: backendName == "macos-native-player"
        ? "macOS shared native DecodeThread facade"
        : "macOS Synthetic FlutterTexture",
      decoderName: backendName == "macos-native-player"
        ? nativePlayer?.decoderName() ?? "decode_thread_software"
        : "synthetic"
    )
    tracks.append(track)
    markFrameAvailable()
    return track
  }

  private func removeTrack(arguments: Any?) {
    guard let fileId = intArg(arguments, "fileId") else { return }
    if backendName == "macos-native-player" {
      if fileId == 0 {
        destroyPlayer()
        return
      }
      nativePlayer?.removeTrack(fileId: fileId)
    }
    tracks.removeAll { intValue($0["fileId"]) == fileId }
    if backendName != "macos-native-player" {
      tracks = tracks.enumerated().map { index, track in
        var next = track
        next["slot"] = index
        return next
      }
    }
    currentDurationUs = tracks
      .map { intValue($0["durationUs"]) ?? 0 }
      .max() ?? 0
    if tracks.isEmpty {
      destroyPlayer()
    } else {
      refreshCurrentFrameAfterLayoutChange()
    }
  }

  private func resize(arguments: Any?) {
    let width = intArg(arguments, "width")
    let height = intArg(arguments, "height")
    if let width, let height {
      let nextWidth = max(16, width)
      let nextHeight = max(16, height)
      let currentDimensions = texture?.dimensions()
      let willChange = currentDimensions?.width != nextWidth ||
        currentDimensions?.height != nextHeight
      if backendName == "macos-native-player", willChange {
        nativePlayer?.clearMetalPresentationTarget()
      }
      _ = texture?.resize(width: nextWidth, height: nextHeight) ?? false
      if backendName == "macos-native-player" {
        refreshCurrentFrameAfterLayoutChange()
        if isPlaying,
           let nativePlayer,
           let texture,
           texture.installNativePresentationTarget(
             nativePlayer,
             maxTrackSlots: activeTrackSlotCapacity()
           ) {
          // The next scheduler callback will publish the next renderer-owned frame.
        }
      }
    }
    markFrameAvailable()
  }

  private func captureViewport() -> Any {
    guard let texture else {
      return FlutterError(
        code: "NO_PLAYER",
        message: "No macOS Flutter texture bridge is registered",
        details: nil
      )
    }
    let metrics = texture.captureMetrics()

    return [
      "hash": metrics.hash,
      "width": metrics.width,
      "height": metrics.height,
      "avgLuma": metrics.avgLuma,
      "nonBlackRatio": metrics.nonBlackRatio,
    ]
  }

  private func markFrameAvailable() {
    if let id = textureId {
      textureRegistry.textureFrameAvailable(id)
    }
  }

  private func activeDurationUs() -> Int {
    currentDurationUs > 0 ? currentDurationUs : Self.syntheticDurationUs
  }

  private func activeTrackSlotCapacity() -> Int {
    let maxSlot = tracks
      .compactMap { intValue($0["slot"]) }
      .max() ?? 0
    return max(1, min(4, maxSlot + 1))
  }

  private func refreshDecodedFrameIfNeeded(targetPtsUs: Int) -> FlutterError? {
    guard backendName == "macos-native-player",
          let nativePlayer,
          texture != nil else {
      return nil
    }

    do {
      nativePlayer.seek(targetPtsUs)
      guard let texture else {
        throw MacOSNativePlayerError.invalidPayload
      }
      let frameInfo = try texture.updateFromNativePlayer(
        nativePlayer,
        maxTrackSlots: activeTrackSlotCapacity(),
        waitTimeoutMs: 3_000
      )
      publishFrameInfo(frameInfo)
      return nil
    } catch {
      return FlutterError(
        code: "DECODE_FAILED",
        message: "Failed to decode macOS video frame",
        details: "\(error)"
      )
    }
  }

  private func refreshCurrentFrameAfterLayoutChange() {
    guard backendName == "macos-native-player",
          let nativePlayer,
          let texture else {
      markFrameAvailable()
      return
    }
    do {
      let frameInfo = try texture.updateFromNativePlayer(
        nativePlayer,
        maxTrackSlots: activeTrackSlotCapacity(),
        waitTimeoutMs: 100
      )
      publishFrameInfo(frameInfo)
    } catch {
      NSLog("VoidPlayer macOS native layout refresh failed: \(error)")
    }
    markFrameAvailable()
  }

  private func seekAndRefresh(
    targetPtsUs: Int,
    requestId: Int?,
    resumeAfterSeek: Bool
  ) -> FlutterError? {
    stopNativeFramePump()
    nativePlayer?.pause()
    isPlaying = false
    currentPtsUs = max(0, min(activeDurationUs(), targetPtsUs))
    if let error = refreshDecodedFrameIfNeeded(targetPtsUs: currentPtsUs) {
      return error
    }
    markFrameAvailable()
    emitSeekPreviewPresented(requestId: requestId, targetPtsUs: currentPtsUs)
    if resumeAfterSeek {
      isPlaying = textureId != nil
      nativePlayer?.play()
      startNativeFramePump()
    }
    return nil
  }

  private func stepAndRefresh(forward: Bool) -> FlutterError? {
    stopNativeFramePump()
    isPlaying = false
    guard let nativePlayer,
          let texture else {
      return nil
    }
    do {
      if forward {
        try nativePlayer.stepForward()
      } else {
        try nativePlayer.stepBackward()
      }
      let frameInfo = try texture.updateFromNativePlayer(
        nativePlayer,
        maxTrackSlots: activeTrackSlotCapacity(),
        waitTimeoutMs: 3_000
      )
      publishFrameInfo(frameInfo)
    } catch {
      return FlutterError(
        code: "STEP_FAILED",
        message: "Failed to step macOS native playback",
        details: "\(error)"
      )
    }
    markFrameAvailable()
    return nil
  }

  private func publishFrameInfo(_ info: MacOSNativeFrameInfo) {
    currentPtsUs = info.ptsUs
    lastPresentedPtsUs = info.ptsUs
    lastPresentedDtsUs = normalizedDtsUs(info)
    lastPresentedDurationUs = info.durationUs
    recordPresentedPts(info.ptsUs)
  }

  private func emitSeekPreviewPresented(requestId: Int?, targetPtsUs: Int) {
    guard let requestId,
          let ptsUs = lastPresentedPtsUs else {
      return
    }
    guard let eventSink else {
      nativeEventDropNoSinkCount += 1
      return
    }
    nativeEventSequence += 1
    nativeEventEmitCount += 1
    let payload: [String: Any] = [
      "schemaVersion": 1,
      "sequence": nativeEventSequence,
      "type": "seekPreviewPresented",
      "timestampUs": Int(Date().timeIntervalSince1970 * 1_000_000),
      "requestId": requestId,
      "trackFileId": 0,
      "ptsUs": ptsUs,
      "dtsUs": lastPresentedDtsUs ?? ptsUs,
      "targetPtsUs": targetPtsUs,
    ]
    DispatchQueue.main.async {
      eventSink(payload)
    }
  }

  private func resetNativeFrameCounters() {
    nativeFrameCallbackCount = 0
    nativeFrameCopyCount = 0
    nativeFrameRendererOwnedPresentCount = 0
    nativeFrameSwiftCopyCount = 0
    nativeFrameCopyMissCount = 0
    nativeFrameCopyErrorCount = 0
    nativeFrameCopyCoalescedCount = 0
    nativeFrameCopyInFlight = false
    nativeFrameCopyFirstHostNs = nil
    nativeFrameCopyLastHostNs = nil
    resetPresentedPtsTrace()
  }

  private func resetPresentedPtsTrace() {
    presentedPtsTrace.removeAll(keepingCapacity: true)
    presentedPtsSampleCount = 0
    presentedPtsDistinctCount = 0
    presentedPtsFirstUs = nil
    presentedPtsLastStepUs = 0
    presentedPtsMonotonicViolationCount = 0
  }

  private func recordPresentedPts(_ ptsUs: Int) {
    if let last = presentedPtsTrace.last {
      let step = ptsUs - last
      presentedPtsLastStepUs = step
      if step < 0 {
        presentedPtsMonotonicViolationCount += 1
      }
      if step != 0 {
        presentedPtsDistinctCount += 1
      }
    } else {
      presentedPtsDistinctCount = 1
    }
    if presentedPtsFirstUs == nil {
      presentedPtsFirstUs = ptsUs
    }
    presentedPtsSampleCount += 1
    presentedPtsTrace.append(ptsUs)
    if presentedPtsTrace.count > presentedPtsTraceCapacity {
      presentedPtsTrace.removeFirst(presentedPtsTrace.count - presentedPtsTraceCapacity)
    }
  }

  private func presentedPtsAdvanceUs() -> Int {
    guard let first = presentedPtsFirstUs,
          let last = lastPresentedPtsUs else {
      return 0
    }
    return max(0, last - first)
  }

  private func nativeFrameCopyElapsedMs() -> Int {
    guard let first = nativeFrameCopyFirstHostNs,
          let last = nativeFrameCopyLastHostNs,
          last >= first else {
      return 0
    }
    return Int((last - first) / 1_000_000)
  }

  private func nativeFrameCopyFps() -> Double {
    let elapsedMs = nativeFrameCopyElapsedMs()
    guard nativeFrameCopyCount > 1, elapsedMs > 0 else {
      return 0.0
    }
    return Double(nativeFrameCopyCount - 1) * 1000.0 / Double(elapsedMs)
  }

  private func presentationFallbackReason(perfStats: [String: Any]?) -> String {
    guard let player = nativePlayer else {
      return "no-native-player"
    }
    if !player.hardwareDecodeActive() {
      return "software-decode"
    }
    if player.hardwareDecodeDownloadsToCpu() {
      return "hardware-download-to-cpu"
    }
    if !nativePresentationTargetInstalled {
      return "native-presentation-target-unavailable"
    }
    let uploadCount = int64Diagnostic(perfStats?["rendererOwnedUploadCount"])
    let failureCount = int64Diagnostic(perfStats?["rendererOwnedUploadFailureCount"])
    if uploadCount == 0 && failureCount > 0 {
      return "renderer-owned-upload-failed"
    }
    return "none"
  }

  private func int64Diagnostic(_ value: Any?) -> Int64 {
    switch value {
    case let value as Int64:
      return value
    case let value as Int:
      return Int64(value)
    case let value as UInt64:
      return Int64(min(value, UInt64(Int64.max)))
    case let value as Double:
      return Int64(value)
    default:
      return 0
    }
  }

  private func normalizedDtsUs(_ info: MacOSNativeFrameInfo) -> Int {
    info.dtsUs == Int.min ? info.ptsUs : info.dtsUs
  }

  private func startNativeFramePump() {
    stopNativeFramePump()
    guard backendName == "macos-native-player",
          let nativePlayer,
          textureId != nil else {
      return
    }

    let generation = playbackGeneration + 1
    playbackGeneration = generation
    resetNativeFrameCounters()
    nativePlayer.resetRendererOwnedPresentationStats()
    nativeFrameCallbackRegistered = true
    nativePresentationTargetInstalled = false
    if let texture {
      nativePresentationTargetInstalled = texture.installNativePresentationTarget(
        nativePlayer,
        maxTrackSlots: activeTrackSlotCapacity()
      )
    }
    nativePlayer.setFrameAvailableCallback(
      macOSNativeFrameAvailable,
      userData: Unmanaged.passUnretained(self).toOpaque()
    )
    let maxTrackSlots = activeTrackSlotCapacity()
    guard !nativePresentationTargetInstalled else {
      return
    }
    nativeFrameCopyInFlight = true
    playbackQueue.async { [weak self] in
      self?.copyNativePlaybackFrame(generation: generation, maxTrackSlots: maxTrackSlots)
    }
  }

  private func stopNativeFramePump() {
    playbackGeneration += 1
    nativeFrameCopyInFlight = false
    nativePlayer?.clearMetalPresentationTarget()
    nativePresentationTargetInstalled = false
    if nativeFrameCallbackRegistered {
      nativePlayer?.setFrameAvailableCallback(nil, userData: nil)
      nativeFrameCallbackRegistered = false
    }
  }

  fileprivate func scheduleNativeFrameCopyFromCallback() {
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      self.nativeFrameCallbackCount += 1
      guard self.isPlaying,
            self.backendName == "macos-native-player" else {
        return
      }
      if self.nativePlayer?.lastRendererOwnedPresentationSucceeded() == true {
        self.nativeFrameCopyCount += 1
        self.nativeFrameRendererOwnedPresentCount += 1
        let now = DispatchTime.now().uptimeNanoseconds
        if self.nativeFrameCopyFirstHostNs == nil {
          self.nativeFrameCopyFirstHostNs = now
        }
        self.nativeFrameCopyLastHostNs = now
        if let frameInfo = self.nativePlayer?.lastRendererOwnedFrameInfo() {
          self.publishFrameInfo(frameInfo)
        }
        self.markFrameAvailable()
        return
      }
      if self.nativePresentationTargetInstalled {
        return
      }
      if self.nativeFrameCopyInFlight {
        self.nativeFrameCopyCoalescedCount += 1
        return
      }
      self.nativeFrameCopyInFlight = true
      let generation = self.playbackGeneration
      let maxTrackSlots = self.activeTrackSlotCapacity()
      self.playbackQueue.async { [weak self] in
        self?.copyNativePlaybackFrame(generation: generation, maxTrackSlots: maxTrackSlots)
      }
    }
  }

  private func copyNativePlaybackFrame(generation: Int, maxTrackSlots: Int) {
    do {
      guard let nativePlayer, let texture else {
        DispatchQueue.main.async { [weak self] in
          guard let self, self.playbackGeneration == generation else { return }
          self.nativeFrameCopyInFlight = false
        }
        return
      }
      let frameInfo = try texture.updateFromNativePlayer(
        nativePlayer,
        maxTrackSlots: maxTrackSlots,
        waitTimeoutMs: 0
      )
      DispatchQueue.main.async { [weak self] in
        guard let self,
              self.playbackGeneration == generation,
              self.isPlaying,
              self.backendName == "macos-native-player" else {
          self?.nativeFrameCopyInFlight = false
          return
        }
        self.nativeFrameCopyInFlight = false
        let now = DispatchTime.now().uptimeNanoseconds
        if self.nativeFrameCopyFirstHostNs == nil {
          self.nativeFrameCopyFirstHostNs = now
        }
        self.nativeFrameCopyLastHostNs = now
        self.nativeFrameCopyCount += 1
        self.nativeFrameSwiftCopyCount += 1
        self.publishFrameInfo(frameInfo)
        self.markFrameAvailable()
      }
    } catch {
      DispatchQueue.main.async { [weak self] in
        guard let self, self.playbackGeneration == generation else { return }
        self.nativeFrameCopyInFlight = false
        if (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true,
           self.isPlaying,
           self.backendName == "macos-native-player" {
          self.nativeFrameCopyMissCount += 1
          return
        }
        self.nativeFrameCopyErrorCount += 1
        NSLog("VoidPlayer macOS native playback frame copy failed: \(error)")
        self.isPlaying = false
        self.nativePlayer?.pause()
        self.stopNativeFramePump()
      }
    }
  }

  private func trackMap(
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

  private static func defaultLayout() -> [String: Any] {
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

  func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
    eventSink = events
    nativeEventListenCount += 1
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
    eventSink = nil
    return nil
  }

  private func pickFiles(arguments: Any?, result: @escaping FlutterResult) {
    let allowsMultipleSelection = boolArg(arguments, "allowMultiple") ?? true
    DispatchQueue.main.async {
      let panel = NSOpenPanel()
      panel.canChooseFiles = true
      panel.canChooseDirectories = false
      panel.allowsMultipleSelection = allowsMultipleSelection
      panel.resolvesAliases = true

      panel.begin { response in
        if response == .OK {
          result(panel.urls.map(\.path))
        } else {
          result(nil)
        }
      }
    }
  }

  private func intArg(_ arguments: Any?, _ key: String) -> Int? {
    guard let map = arguments as? [String: Any] else { return nil }
    return intValue(map[key])
  }

  private func boolArg(_ arguments: Any?, _ key: String) -> Bool? {
    guard let map = arguments as? [String: Any] else { return nil }
    if let value = map[key] as? Bool {
      return value
    }
    if let value = map[key] as? NSNumber {
      return value.boolValue
    }
    return nil
  }

  private func doubleArg(_ arguments: Any?, _ key: String) -> Double? {
    guard let map = arguments as? [String: Any] else { return nil }
    return doubleValue(map[key])
  }

  private func doubleValue(_ value: Any?) -> Double? {
    if let value = value as? Double {
      return value
    }
    if let value = value as? NSNumber {
      return value.doubleValue
    }
    return nil
  }

  private func stringArg(_ arguments: Any?, _ key: String) -> String? {
    guard let map = arguments as? [String: Any] else { return nil }
    return map[key] as? String
  }

  private func stringListArg(_ arguments: Any?, _ key: String) -> [String] {
    guard let map = arguments as? [String: Any] else { return [] }
    if let values = map[key] as? [String] {
      return values
    }
    if let values = map[key] as? [Any] {
      return values.compactMap { $0 as? String }
    }
    return []
  }

  private func intValue(_ value: Any?) -> Int? {
    if let value = value as? Int {
      return value
    }
    if let value = value as? Int64 {
      return Int(value)
    }
    if let value = value as? NSNumber {
      return value.intValue
    }
    return nil
  }

  private func intListValue(_ value: Any?) -> [Int] {
    if let values = value as? [Int] {
      return values
    }
    if let values = value as? [Any] {
      return values.compactMap { intValue($0) }
    }
    return [0, 1, 2, 3]
  }
}

private final class MacOSFlutterTextureBridge: NSObject, FlutterTexture {
  private let lock = NSLock()
  private(set) var width: Int
  private(set) var height: Int
  private let isSyntheticSource: Bool
  private let metalUploadEnabled: Bool
  private var nativeMetalPresentationBackend: OpaquePointer?
  private let hashPrefix: String
  private var pixelBuffer: CVPixelBuffer?
  private var pixelBufferBackBuffer: CVPixelBuffer?
  private var pixelBufferRetiredBuffer: CVPixelBuffer?
  private var pixelBufferRebuildCount = 0
  private var pixelBufferReuseCount = 0
  private var pixelBufferDirectCopyCount = 0
  private var pixelBufferMetalUploadCount = 0
  private var pixelBufferMetalUploadFailureCount = 0
  private var metalTextureValid = false
  private var metalTextureCreationCount = 0
  private var metalTextureFailureCount = 0
  private var metalTextureLastError = ""

  init(width: Int, height: Int, metalUploadEnabled: Bool) {
    self.width = width
    self.height = height
    self.isSyntheticSource = true
    self.metalUploadEnabled = metalUploadEnabled
    self.hashPrefix = "macos-synthetic"
    super.init()
    createNativeMetalPresentationBackend()
    rebuildPixelBuffer()
  }

  init(nativeWidth: Int, nativeHeight: Int, metalUploadEnabled: Bool) {
    self.width = nativeWidth
    self.height = nativeHeight
    self.isSyntheticSource = false
    self.metalUploadEnabled = metalUploadEnabled
    self.hashPrefix = "macos-native-frame"
    super.init()
    createNativeMetalPresentationBackend()
    rebuildPixelBuffer()
  }

  deinit {
    if let nativeMetalPresentationBackend {
      VPMacOSMetalPresentationBackendDestroy(nativeMetalPresentationBackend)
    }
  }

  func resize(width: Int, height: Int) -> Bool {
    lock.lock()
    defer { lock.unlock() }

    guard width != self.width || height != self.height else { return false }
    self.width = width
    self.height = height
    createNativeMetalPresentationBackendLocked()
    rebuildPixelBufferLocked()
    return true
  }

  func updateFromNativePlayer(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    waitTimeoutMs: Int
  ) throws -> MacOSNativeFrameInfo {
    lock.lock()
    defer { lock.unlock() }

    guard !isSyntheticSource else {
      throw MacOSNativePlayerError.invalidPayload
    }
    if pixelBuffer == nil {
      rebuildPixelBufferLocked()
    }
    guard let pixelBuffer else {
      throw MacOSNativePlayerError.invalidPayload
    }

    if metalUploadEnabled {
      if let info = try copyFromNativePlayerWithMetalUpload(
        player,
        pixelBuffer: pixelBuffer,
        maxTrackSlots: maxTrackSlots,
        waitTimeoutMs: waitTimeoutMs
      ) {
        pixelBufferReuseCount += 1
        return info
      }
    }

    guard let targetBuffer = ensureBackPixelBufferLocked() else {
      throw MacOSNativePlayerError.invalidPayload
    }

    CVPixelBufferLockBaseAddress(targetBuffer, [])
    guard let baseAddress = CVPixelBufferGetBaseAddress(targetBuffer) else {
      CVPixelBufferUnlockBaseAddress(targetBuffer, [])
      throw MacOSNativePlayerError.invalidPayload
    }
    let bytesPerRow = CVPixelBufferGetBytesPerRow(targetBuffer)
    let dstSize = bytesPerRow * height
    let info: MacOSNativeFrameInfo
    do {
      info = try player.copyPresentationIntoBGRA(
        baseAddress.assumingMemoryBound(to: UInt8.self),
        dstSize: dstSize,
        width: width,
        height: height,
        strideBytes: bytesPerRow,
        waitTimeoutMs: waitTimeoutMs
      )
    } catch {
      CVPixelBufferUnlockBaseAddress(targetBuffer, [])
      throw error
    }
    CVPixelBufferUnlockBaseAddress(targetBuffer, [])
    publishBackBufferLocked(targetBuffer)
    validateMetalTextureLocked(buffer: targetBuffer)
    pixelBufferReuseCount += 1
    pixelBufferDirectCopyCount += 1
    return info
  }

  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    lock.lock()
    defer { lock.unlock() }

    guard let pixelBuffer else { return nil }
    return Unmanaged.passRetained(pixelBuffer)
  }

  func dimensions() -> (width: Int, height: Int) {
    lock.lock()
    defer { lock.unlock() }

    return (width: width, height: height)
  }

  func captureMetrics() -> (
    width: Int,
    height: Int,
    avgLuma: Double,
    nonBlackRatio: Double,
    hash: String
  ) {
    lock.lock()
    defer { lock.unlock() }

    guard let pixelBuffer else {
      return (
        width: width,
        height: height,
        avgLuma: 0.0,
        nonBlackRatio: 0.0,
        hash: "\(hashPrefix)-empty"
      )
    }
    return measure(buffer: pixelBuffer)
  }

  func diagnostics() -> (
    rebuildCount: Int,
    reuseCount: Int,
    directCopyCount: Int,
    metalUploadCount: Int,
    metalYuvUploadCount: Int,
    metalCVPixelBufferUploadCount: Int,
    metalUploadFailureCount: Int,
    metalUploadEnabled: Bool,
    presentationUploadMode: String,
    presentPackageUploadCount: Int,
    presentPackageCopyUs: Int,
    presentPackageGpuWaitUs: Int,
    presentPackageTotalUs: Int,
    presentPackageStorage: String,
    metalAvailable: Bool,
    metalTextureCacheAvailable: Bool,
    metalTextureValid: Bool,
    metalTextureCreationCount: Int,
    metalTextureFailureCount: Int,
    metalTextureLastError: String
  ) {
    lock.lock()
    defer { lock.unlock() }

    return (
      rebuildCount: pixelBufferRebuildCount,
      reuseCount: pixelBufferReuseCount,
      directCopyCount: pixelBufferDirectCopyCount,
      metalUploadCount: max(
        pixelBufferMetalUploadCount,
        nativeMetalPresentPackageUploadCountLocked()
      ),
      metalYuvUploadCount: nativeMetalUploaderDirectYuvUploadCountLocked(),
      metalCVPixelBufferUploadCount: nativeMetalUploaderCVPixelBufferUploadCountLocked(),
      metalUploadFailureCount: pixelBufferMetalUploadFailureCount,
      metalUploadEnabled: metalUploadEnabled,
      presentationUploadMode: presentationUploadModeLocked(),
      presentPackageUploadCount: nativeMetalPresentPackageUploadCountLocked(),
      presentPackageCopyUs: nativeMetalLastPresentPackageCopyUsLocked(),
      presentPackageGpuWaitUs: nativeMetalLastPresentPackageGpuWaitUsLocked(),
      presentPackageTotalUs: nativeMetalLastPresentPackageTotalUsLocked(),
      presentPackageStorage: nativeMetalLastPresentPackageStorageLocked(),
      metalAvailable: nativeMetalUploaderAvailableLocked(),
      metalTextureCacheAvailable: nativeMetalUploaderAvailableLocked(),
      metalTextureValid: metalTextureValid,
      metalTextureCreationCount: metalTextureCreationCount,
      metalTextureFailureCount: metalTextureFailureCount,
      metalTextureLastError: metalTextureLastError
    )
  }

  func installNativePresentationTarget(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int
  ) -> Bool {
    lock.lock()
    defer { lock.unlock() }

    guard !isSyntheticSource,
          metalUploadEnabled,
          let nativeMetalPresentationBackend,
          let pixelBuffer,
          nativeMetalUploaderAvailableLocked(),
          metalTextureValid else {
      return false
    }
    return player.setMetalPresentationTarget(
      backend: nativeMetalPresentationBackend,
      pixelBuffer: pixelBuffer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots
    )
  }

  private func rebuildPixelBuffer() {
    lock.lock()
    defer { lock.unlock() }

    rebuildPixelBufferLocked()
  }

  private func rebuildPixelBufferLocked() {
    let attributes = [
      kCVPixelBufferCGImageCompatibilityKey as String: true,
      kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
      kCVPixelBufferMetalCompatibilityKey as String: true,
      kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ] as CFDictionary

    guard let nextBuffer = makePixelBufferLocked(attributes: attributes) else {
      pixelBuffer = nil
      pixelBufferBackBuffer = nil
      pixelBufferRetiredBuffer = nil
      return
    }
    pixelBufferRebuildCount += 1

    fill(buffer: nextBuffer)
    pixelBuffer = nextBuffer
    pixelBufferBackBuffer = makePixelBufferLocked(attributes: attributes)
    pixelBufferRetiredBuffer = makePixelBufferLocked(attributes: attributes)
    validateMetalTextureLocked(buffer: nextBuffer)
  }

  private func makePixelBufferLocked(attributes: CFDictionary) -> CVPixelBuffer? {
    var nextBuffer: CVPixelBuffer?
    let status = CVPixelBufferCreate(
      kCFAllocatorDefault,
      width,
      height,
      kCVPixelFormatType_32BGRA,
      attributes,
      &nextBuffer
    )
    guard status == kCVReturnSuccess else { return nil }
    return nextBuffer
  }

  private func makePixelBufferLocked() -> CVPixelBuffer? {
    let attributes = [
      kCVPixelBufferCGImageCompatibilityKey as String: true,
      kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
      kCVPixelBufferMetalCompatibilityKey as String: true,
      kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ] as CFDictionary
    return makePixelBufferLocked(attributes: attributes)
  }

  private func ensureBackPixelBufferLocked() -> CVPixelBuffer? {
    if let pixelBufferBackBuffer,
       CVPixelBufferGetWidth(pixelBufferBackBuffer) == width,
       CVPixelBufferGetHeight(pixelBufferBackBuffer) == height {
      return pixelBufferBackBuffer
    }
    pixelBufferBackBuffer = makePixelBufferLocked()
    return pixelBufferBackBuffer
  }

  private func publishBackBufferLocked(_ buffer: CVPixelBuffer) {
    let previousFront = pixelBuffer
    let previousRetired = pixelBufferRetiredBuffer
    pixelBuffer = buffer
    pixelBufferRetiredBuffer = previousFront
    if let previousFront,
       CVPixelBufferGetWidth(previousFront) != width ||
       CVPixelBufferGetHeight(previousFront) != height {
      pixelBufferRetiredBuffer = nil
    }
    if let previousRetired,
       CVPixelBufferGetWidth(previousRetired) == width,
       CVPixelBufferGetHeight(previousRetired) == height {
      pixelBufferBackBuffer = previousRetired
    } else {
      pixelBufferBackBuffer = makePixelBufferLocked()
    }
  }

  private func createNativeMetalPresentationBackend() {
    lock.lock()
    defer { lock.unlock() }
    createNativeMetalPresentationBackendLocked()
  }

  private func createNativeMetalPresentationBackendLocked() {
    if let nativeMetalPresentationBackend {
      VPMacOSMetalPresentationBackendDestroy(nativeMetalPresentationBackend)
    }
    nativeMetalPresentationBackend = VPMacOSMetalPresentationBackendCreate(
      Int32(width),
      Int32(height)
    )
  }

  private func nativeMetalUploaderAvailableLocked() -> Bool {
    guard let nativeMetalPresentationBackend else { return false }
    return VPMacOSMetalPresentationBackendIsAvailable(nativeMetalPresentationBackend) != 0
  }

  private func nativeMetalUploaderDirectYuvUploadCountLocked() -> Int {
    guard let nativeMetalPresentationBackend else { return 0 }
    return Int(
      VPMacOSMetalPresentationBackendDirectYUVUploadCount(nativeMetalPresentationBackend)
    )
  }

  private func nativeMetalUploaderCVPixelBufferUploadCountLocked() -> Int {
    guard let nativeMetalPresentationBackend else { return 0 }
    return Int(
      VPMacOSMetalPresentationBackendCVPixelBufferUploadCount(nativeMetalPresentationBackend)
    )
  }

  private func nativeMetalPresentPackageUploadCountLocked() -> Int {
    guard let nativeMetalPresentationBackend else { return 0 }
    return Int(
      VPMacOSMetalPresentationBackendPresentPackageUploadCount(nativeMetalPresentationBackend)
    )
  }

  private func nativeMetalLastPresentPackageCopyUsLocked() -> Int {
    guard let nativeMetalPresentationBackend else { return 0 }
    return Int(
      VPMacOSMetalPresentationBackendLastPresentPackageCopyUs(nativeMetalPresentationBackend)
    )
  }

  private func nativeMetalLastPresentPackageGpuWaitUsLocked() -> Int {
    guard let nativeMetalPresentationBackend else { return 0 }
    return Int(
      VPMacOSMetalPresentationBackendLastPresentPackageGpuWaitUs(nativeMetalPresentationBackend)
    )
  }

  private func nativeMetalLastPresentPackageTotalUsLocked() -> Int {
    guard let nativeMetalPresentationBackend else { return 0 }
    return Int(
      VPMacOSMetalPresentationBackendLastPresentPackageTotalUs(nativeMetalPresentationBackend)
    )
  }

  private func nativeMetalLastPresentPackageStorageLocked() -> String {
    guard let nativeMetalPresentationBackend else { return "unavailable" }
    let storage = VPMacOSMetalPresentationBackendLastPresentPackageStorage(
      nativeMetalPresentationBackend
    )
    switch storage {
    case Int32(VPMacOSNativePresentPackageStorageYUV):
      return "yuv"
    case Int32(VPMacOSNativePresentPackageStorageBGRA):
      return "bgra"
    case Int32(VPMacOSNativePresentPackageStorageCVPixelBuffer):
      return "cvpixelbuffer"
    default:
      return "unavailable"
    }
  }

  private func presentationUploadModeLocked() -> String {
    if metalUploadEnabled, metalTextureValid, nativeMetalUploaderAvailableLocked() {
      return "metal-bgra-layout-upload"
    }
    if pixelBuffer != nil {
      return "cvpixelbuffer-direct-copy"
    }
    return "unavailable"
  }

  private func copyFromNativePlayerWithMetalUpload(
    _ player: MacOSNativePlayerSession,
    pixelBuffer: CVPixelBuffer,
    maxTrackSlots: Int,
    waitTimeoutMs: Int
  ) throws -> MacOSNativeFrameInfo? {
    guard metalUploadEnabled else {
      return nil
    }
    guard let nativeMetalPresentationBackend,
          VPMacOSMetalPresentationBackendIsAvailable(nativeMetalPresentationBackend) != 0 else {
      return nil
    }
    do {
      guard let info = try player.copyCurrentFrameToMetalPixelBuffer(
        backend: nativeMetalPresentationBackend,
        pixelBuffer: pixelBuffer,
        width: width,
        height: height,
        maxTrackSlots: maxTrackSlots,
        waitTimeoutMs: waitTimeoutMs
      ) else {
        pixelBufferMetalUploadFailureCount += 1
        return nil
      }
      pixelBufferMetalUploadCount += 1
      validateMetalTextureLocked(buffer: pixelBuffer)
      return info
    } catch {
      pixelBufferMetalUploadFailureCount += 1
      NSLog("VoidPlayer macOS Metal layout upload failed; falling back to CPU BGRA layout: \(error)")
      return nil
    }
  }

  private func validateMetalTextureLocked(buffer: CVPixelBuffer) {
    guard let nativeMetalPresentationBackend else {
      metalTextureValid = false
      metalTextureLastError = "native Metal presentation backend is null"
      return
    }

    var error = [CChar](repeating: 0, count: 512)
    let status = VPMacOSMetalPresentationBackendValidatePixelBufferChecked(
      nativeMetalPresentationBackend,
      UnsafeMutableRawPointer(Unmanaged.passUnretained(buffer).toOpaque()),
      Int32(width),
      Int32(height),
      &error,
      error.count
    )
    if status == 0 {
      metalTextureCreationCount += 1
      metalTextureValid = true
      metalTextureLastError = ""
    } else {
      metalTextureFailureCount += 1
      metalTextureValid = false
      metalTextureLastError = String(cString: error)
    }
  }

  private func measure(buffer: CVPixelBuffer) -> (
    width: Int,
    height: Int,
    avgLuma: Double,
    nonBlackRatio: Double,
    hash: String
  ) {
    CVPixelBufferLockBaseAddress(buffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }

    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else {
      return (
        width: width,
        height: height,
        avgLuma: 0.0,
        nonBlackRatio: 0.0,
        hash: "\(hashPrefix)-empty"
      )
    }

    let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)
    let pixels = baseAddress.assumingMemoryBound(to: UInt8.self)
    var metrics = VPMacOSCaptureMetrics()
    let ret = VPMacOSMeasureBGRA(
      pixels,
      Int32(width),
      Int32(height),
      Int32(bytesPerRow),
      &metrics
    )
    guard ret == 0 else {
      return (
        width: width,
        height: height,
        avgLuma: 0.0,
        nonBlackRatio: 0.0,
        hash: "\(hashPrefix)-empty"
      )
    }

    return (
      width: Int(metrics.width),
      height: Int(metrics.height),
      avgLuma: metrics.avg_luma,
      nonBlackRatio: metrics.non_black_ratio,
      hash: String(format: "%@-%dx%d-%016llx", hashPrefix, width, height, metrics.hash)
    )
  }

  private func fill(buffer: CVPixelBuffer) {
    CVPixelBufferLockBaseAddress(buffer, [])
    defer { CVPixelBufferUnlockBaseAddress(buffer, []) }

    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else { return }
    let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)
    let pixels = baseAddress.assumingMemoryBound(to: UInt8.self)
    let barWidth = max(1, width / 6)

    for y in 0..<height {
      for x in 0..<width {
        let bar = min(5, x / barWidth)
        let gradient = UInt8((x * 63) / max(1, width - 1))
        let scanline = UInt8((y * 31) / max(1, height - 1))
        let color = colorForBar(bar, gradient: gradient, scanline: scanline)
        let offset = y * bytesPerRow + x * 4
        pixels[offset + 0] = color.b
        pixels[offset + 1] = color.g
        pixels[offset + 2] = color.r
        pixels[offset + 3] = 255
      }
    }
  }

  private func copyBGRA(_ data: Data, to buffer: CVPixelBuffer) {
    CVPixelBufferLockBaseAddress(buffer, [])
    defer { CVPixelBufferUnlockBaseAddress(buffer, []) }

    copyBGRALocked(data, to: buffer)
  }

  private func copyBGRALocked(_ data: Data, to buffer: CVPixelBuffer) {
    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else { return }
    let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)
    let rowBytes = width * 4

    data.withUnsafeBytes { rawSource in
      guard let source = rawSource.baseAddress else { return }
      for y in 0..<height {
        let sourceOffset = y * rowBytes
        guard sourceOffset < rawSource.count else { break }
        let sourceRowBytes = min(rowBytes, rawSource.count - sourceOffset)
        memcpy(
          baseAddress.advanced(by: y * bytesPerRow),
          source.advanced(by: sourceOffset),
          sourceRowBytes
        )
      }
    }
  }

  private func colorForBar(
    _ bar: Int,
    gradient: UInt8,
    scanline: UInt8
  ) -> (b: UInt8, g: UInt8, r: UInt8) {
    switch bar {
    case 0:
      return (32, 48 &+ scanline, 220)
    case 1:
      return (32 &+ gradient, 180, 64)
    case 2:
      return (220, 180 &- min(scanline, 120), 48)
    case 3:
      return (180, 64 &+ gradient, 200)
    case 4:
      return (64, 210, 210 &- min(gradient, 160))
    default:
      return (200 &- min(scanline, 120), 88, 96 &+ gradient)
    }
  }
}

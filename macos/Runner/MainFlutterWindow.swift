import Cocoa
import CoreVideo
import FlutterMacOS

private struct MacOSDecodedFirstFrame {
  let width: Int
  let height: Int
  let durationUs: Int
  let ptsUs: Int
  let dtsUs: Int
  let bgra: Data
}

private struct MacOSNativeFrameInfo {
  let width: Int
  let height: Int
  let durationUs: Int
  let ptsUs: Int
  let dtsUs: Int
}

private enum MacOSNativePlayerError: Error, CustomStringConvertible {
  case failed(String)
  case invalidPayload

  var description: String {
    switch self {
    case .failed(let message):
      return message
    case .invalidPayload:
      return "decoded first frame had invalid dimensions or pixel data"
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

  func close() {
    VPMacOSNativePlayerClose(handle)
  }

  func setFrameAvailableCallback(
    _ callback: VPMacOSFrameAvailableCallback?,
    userData: UnsafeMutableRawPointer?
  ) {
    VPMacOSNativePlayerSetFrameAvailableCallback(handle, callback, userData)
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

  func seek(_ ptsUs: Int) {
    VPMacOSNativePlayerSeek(handle, Int64(ptsUs))
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

  func copyCurrentFrame(waitTimeoutMs: Int = 0) throws -> MacOSDecodedFirstFrame {
    let deadline = Date().addingTimeInterval(Double(waitTimeoutMs) / 1000.0)
    var lastError = ""

    repeat {
      var frame = VPMacOSNativeFrame()
      var error = [CChar](repeating: 0, count: 1024)
      let ret = VPMacOSNativePlayerCopyCurrentFrameBGRA(
        handle,
        &frame,
        &error,
        error.count
      )
      if ret == 0 {
        defer {
          VPMacOSNativeFrameFree(&frame)
        }
        guard frame.width > 0,
              frame.height > 0,
              let bgra = frame.bgra,
              frame.bgra_size > 0 else {
          throw MacOSNativePlayerError.invalidPayload
        }

        return MacOSDecodedFirstFrame(
          width: Int(frame.width),
          height: Int(frame.height),
          durationUs: Int(frame.duration_us),
          ptsUs: Int(frame.pts_us),
          dtsUs: Int(frame.dts_us),
          bgra: Data(bytes: bgra, count: Int(frame.bgra_size))
        )
      }
      let message = String(cString: error)
      lastError = message.isEmpty ? "copyCurrentFrame failed with code \(ret)" : message
      Thread.sleep(forTimeInterval: 0.01)
    } while Date() < deadline

    throw MacOSNativePlayerError.failed(
      lastError.isEmpty ? "timed out waiting for a decoded frame" : lastError
    )
  }

  func copyCurrentFrameIntoBGRA(
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
      let ret = VPMacOSNativePlayerCopyCurrentFrameBGRAInto(
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
        ? "copyCurrentFrameIntoBGRA failed with code \(ret)"
        : message
      Thread.sleep(forTimeInterval: 0.01)
    } while Date() < deadline

    throw MacOSNativePlayerError.failed(
      lastError.isEmpty ? "timed out waiting for a decoded frame" : lastError
    )
  }

  func copyCurrentFrameToMetalPixelBuffer(
    uploader: OpaquePointer,
    pixelBuffer: CVPixelBuffer,
    width: Int,
    height: Int,
    waitTimeoutMs: Int = 0
  ) throws -> MacOSNativeFrameInfo? {
    let deadline = Date().addingTimeInterval(Double(waitTimeoutMs) / 1000.0)
    var lastError = ""

    repeat {
      var frameInfo = VPMacOSNativeFrameInfo()
      var error = [CChar](repeating: 0, count: 1024)
      let ret = VPMacOSMetalUploaderCopyCurrentFrame(
        uploader,
        handle,
        UnsafeMutableRawPointer(Unmanaged.passUnretained(pixelBuffer).toOpaque()),
        Int32(width),
        Int32(height),
        Int32(waitTimeoutMs),
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
      if ret == -2 {
        return nil
      }
      let message = String(cString: error)
      lastError = message.isEmpty
        ? "copyCurrentFrameToMetalPixelBuffer failed with code \(ret)"
        : message
      Thread.sleep(forTimeInterval: 0.01)
    } while Date() < deadline

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
  private var texture: MacOSSyntheticTexture?
  private var textureId: Int64?
  private var tracks: [[String: Any]] = []
  private var layout: [String: Any] = MacOSVideoRendererStub.defaultLayout()
  private var currentPtsUs = 0
  private var currentDurationUs = 0
  private var lastPresentedPtsUs: Int?
  private var lastPresentedDtsUs: Int?
  private var isPlaying = false
  private var backendName = "synthetic-texture"
  private var nativePlayer: MacOSNativePlayerSession?
  private var playbackSpeed = 1.0
  private var nativeFrameCallbackRegistered = false
  private var playbackGeneration = 0

  init(textureRegistry: FlutterTextureRegistry) {
    self.textureRegistry = textureRegistry
    super.init()
  }

  static func register(with engine: FlutterEngine) {
    let stub = MacOSVideoRendererStub(textureRegistry: engine)
    activeInstance = stub
    let messenger = engine.binaryMessenger
    let channel = FlutterMethodChannel(name: channelName, binaryMessenger: messenger)
    channel.setMethodCallHandler(stub.handle)

    let events = FlutterEventChannel(name: eventsChannelName, binaryMessenger: messenger)
    events.setStreamHandler(stub)
  }

  static func destroyActivePlayerForWindowClose() {
    activeInstance?.destroyPlayerForWindowClose()
  }

  private func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "initLogging", "setViewportBackgroundColor", "setTrackOffset":
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
      let resumeAfterSeek = nativePlayer?.isPlaying() ?? isPlaying
      if let error = seekAndRefresh(
        targetPtsUs: targetPtsUs,
        resumeAfterSeek: resumeAfterSeek
      ) {
        result(error)
        return
      }
      result(nil)
    case "stepForward":
      if let error = stepAndRefresh(deltaUs: 33_333) {
        result(error)
        return
      }
      result(nil)
    case "stepBackward":
      if let error = stepAndRefresh(deltaUs: -33_333) {
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
            ]
      )
    case "isPlaying":
      result(nativePlayer?.isPlaying() ?? isPlaying)
    case "getLayout":
      result(layout)
    case "applyLayout":
      if let nextLayout = call.arguments as? [String: Any] {
        layout = nextLayout
      }
      result(nil)
    case "getTracks":
      result(tracks)
    case "pickFiles":
      pickFiles(arguments: call.arguments, result: result)
    case "getDiagnostics":
      let textureStats = texture?.diagnostics()
      let diagnostics: [String: Any] = [
        "platform": "macos",
        "backend": backendName,
        "presentationAdapter": String(cString: VPMacOSNativePresentationAdapterName()),
        "hardwareDecodeProvider": String(cString: VPMacOSNativeHardwareDecodeProviderName()),
        "hardwareDecodeAvailable": VPMacOSNativeHardwareDecodeAvailable() != 0,
        "hardwareDecodeActive": nativePlayer?.hardwareDecodeActive() ?? false,
        "hardwareDecodeDownloadsToCpu": nativePlayer?.hardwareDecodeDownloadsToCpu() ?? false,
        "decodeMode": nativePlayer?.decodeModeName() ?? "none",
        "softwareFallbackActive": nativePlayer?.hardwareDecodeActive() != true,
        "available": nativePlayer != nil,
        "reason": nativePlayer == nil
          ? "Synthetic macOS texture is active"
          : "macOS shared native DecodeThread facade is active",
        "textureId": textureId ?? -1,
        "trackCount": tracks.count,
        "audioAvailable": nativePlayer?.hasAudio() ?? false,
        "audioSampleRate": nativePlayer?.audioSampleRate() ?? 0,
        "audioChannels": nativePlayer?.audioChannels() ?? 0,
        "activeAudioTrack": nativePlayer?.activeAudioTrack() ?? -1,
        "pixelBufferRebuildCount": textureStats?.rebuildCount ?? 0,
        "pixelBufferReuseCount": textureStats?.reuseCount ?? 0,
        "pixelBufferDirectCopyCount": textureStats?.directCopyCount ?? 0,
        "pixelBufferMetalUploadCount": textureStats?.metalUploadCount ?? 0,
        "pixelBufferMetalUploadFailureCount": textureStats?.metalUploadFailureCount ?? 0,
        "pixelBufferMetalUploadEnabled": textureStats?.metalUploadEnabled ?? false,
        "presentationUploadMode": textureStats?.presentationUploadMode ?? "unavailable",
        "metalAvailable": textureStats?.metalAvailable ?? false,
        "metalTextureCacheAvailable": textureStats?.metalTextureCacheAvailable ?? false,
        "metalTextureValid": textureStats?.metalTextureValid ?? false,
        "metalTextureCreationCount": textureStats?.metalTextureCreationCount ?? 0,
        "metalTextureFailureCount": textureStats?.metalTextureFailureCount ?? 0,
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

    let nextTexture: MacOSSyntheticTexture
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
      nextTexture = MacOSSyntheticTexture(
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
        let decoded = try session.copyCurrentFrame(waitTimeoutMs: 3_000)
        nextTexture = MacOSSyntheticTexture(
          decoded: decoded,
          metalUploadEnabled: !Self.metalUploadDisabledForTest
        )
        initialPresentedPtsUs = decoded.ptsUs
        initialPresentedDtsUs = normalizedDtsUs(decoded)
        trackWidth = session.width() > 0 ? session.width() : decoded.width
        trackHeight = session.height() > 0 ? session.height() : decoded.height
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
    currentDurationUs = trackDurationUs
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
    currentPtsUs = initialPresentedPtsUs
    lastPresentedPtsUs = initialPresentedPtsUs
    lastPresentedDtsUs = initialPresentedDtsUs
    isPlaying = false
    markFrameAvailable()

    return [
      "textureId": registeredTextureId,
      "tracks": tracks,
    ]
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
    tracks.removeAll { intValue($0["fileId"]) == fileId }
    tracks = tracks.enumerated().map { index, track in
      var next = track
      next["slot"] = index
      return next
    }
    if tracks.isEmpty {
      destroyPlayer()
    } else {
      markFrameAvailable()
    }
  }

  private func resize(arguments: Any?) {
    let width = intArg(arguments, "width")
    let height = intArg(arguments, "height")
    if let width, let height {
      texture?.resize(width: max(16, width), height: max(16, height))
    }
    markFrameAvailable()
  }

  private func captureViewport() -> Any {
    guard let texture else {
      return FlutterError(
        code: "NO_PLAYER",
        message: "No macOS synthetic texture is registered",
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

  private func refreshDecodedFrameIfNeeded(targetPtsUs: Int) -> FlutterError? {
    guard backendName == "macos-native-player",
          let nativePlayer,
          texture != nil else {
      return nil
    }

    do {
      nativePlayer.seek(targetPtsUs)
      let decoded = try nativePlayer.copyCurrentFrame(waitTimeoutMs: 3_000)
      publishDecodedFrame(decoded)
      return nil
    } catch {
      return FlutterError(
        code: "DECODE_FAILED",
        message: "Failed to decode macOS video frame",
        details: "\(error)"
      )
    }
  }

  private func seekAndRefresh(targetPtsUs: Int, resumeAfterSeek: Bool) -> FlutterError? {
    stopNativeFramePump()
    nativePlayer?.pause()
    isPlaying = false
    currentPtsUs = max(0, min(activeDurationUs(), targetPtsUs))
    if let error = refreshDecodedFrameIfNeeded(targetPtsUs: currentPtsUs) {
      return error
    }
    markFrameAvailable()
    if resumeAfterSeek {
      isPlaying = textureId != nil
      nativePlayer?.play()
      startNativeFramePump()
    }
    return nil
  }

  private func stepAndRefresh(deltaUs: Int) -> FlutterError? {
    stopNativeFramePump()
    nativePlayer?.pause()
    isPlaying = false
    currentPtsUs = nativePlayer?.currentPtsUs() ?? currentPtsUs
    currentPtsUs = max(0, min(activeDurationUs(), currentPtsUs + deltaUs))
    if let error = refreshDecodedFrameIfNeeded(targetPtsUs: currentPtsUs) {
      return error
    }
    markFrameAvailable()
    return nil
  }

  private func publishDecodedFrame(_ decoded: MacOSDecodedFirstFrame) {
    currentPtsUs = decoded.ptsUs
    lastPresentedPtsUs = decoded.ptsUs
    lastPresentedDtsUs = normalizedDtsUs(decoded)
    texture?.update(decoded: decoded)
  }

  private func publishFrameInfo(_ info: MacOSNativeFrameInfo) {
    currentPtsUs = info.ptsUs
    lastPresentedPtsUs = info.ptsUs
    lastPresentedDtsUs = normalizedDtsUs(info)
  }

  private func normalizedDtsUs(_ decoded: MacOSDecodedFirstFrame) -> Int {
    decoded.dtsUs == Int.min ? decoded.ptsUs : decoded.dtsUs
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
    nativeFrameCallbackRegistered = true
    nativePlayer.setFrameAvailableCallback(
      macOSNativeFrameAvailable,
      userData: Unmanaged.passUnretained(self).toOpaque()
    )
    playbackQueue.async { [weak self] in
      self?.copyNativePlaybackFrame(generation: generation)
    }
  }

  private func stopNativeFramePump() {
    playbackGeneration += 1
    if nativeFrameCallbackRegistered {
      nativePlayer?.setFrameAvailableCallback(nil, userData: nil)
      nativeFrameCallbackRegistered = false
    }
  }

  fileprivate func scheduleNativeFrameCopyFromCallback() {
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      let generation = self.playbackGeneration
      self.playbackQueue.async { [weak self] in
        self?.copyNativePlaybackFrame(generation: generation)
      }
    }
  }

  private func copyNativePlaybackFrame(generation: Int) {
    do {
      guard let nativePlayer, let texture else { return }
      let frameInfo = try texture.updateFromNativePlayer(
        nativePlayer,
        waitTimeoutMs: 100
      )
      DispatchQueue.main.async { [weak self] in
        guard let self,
              self.playbackGeneration == generation,
              self.isPlaying,
              self.backendName == "macos-native-player" else {
          return
        }
        self.publishFrameInfo(frameInfo)
        self.markFrameAvailable()
        if frameInfo.ptsUs >= self.activeDurationUs() {
          self.isPlaying = false
          self.nativePlayer?.pause()
          self.stopNativeFramePump()
        }
      }
    } catch {
      DispatchQueue.main.async { [weak self] in
        guard let self, self.playbackGeneration == generation else { return }
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
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
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
    if let value = map[key] as? Double {
      return value
    }
    if let value = map[key] as? NSNumber {
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
}

private final class MacOSSyntheticTexture: NSObject, FlutterTexture {
  private let lock = NSLock()
  private(set) var width: Int
  private(set) var height: Int
  private let syntheticPattern: Bool
  private let metalUploadEnabled: Bool
  private var nativeMetalUploader: OpaquePointer?
  private var decodedBGRA: Data?
  private let hashPrefix: String
  private var pixelBuffer: CVPixelBuffer?
  private var pixelBufferRebuildCount = 0
  private var pixelBufferReuseCount = 0
  private var pixelBufferDirectCopyCount = 0
  private var pixelBufferMetalUploadCount = 0
  private var pixelBufferMetalUploadFailureCount = 0
  private var metalTextureValid = false
  private var metalTextureCreationCount = 0
  private var metalTextureFailureCount = 0

  init(width: Int, height: Int, metalUploadEnabled: Bool) {
    self.width = width
    self.height = height
    self.syntheticPattern = true
    self.metalUploadEnabled = metalUploadEnabled
    self.decodedBGRA = nil
    self.hashPrefix = "macos-synthetic"
    super.init()
    createNativeMetalUploader()
    rebuildPixelBuffer()
  }

  init(decoded: MacOSDecodedFirstFrame, metalUploadEnabled: Bool) {
    self.width = decoded.width
    self.height = decoded.height
    self.syntheticPattern = false
    self.metalUploadEnabled = metalUploadEnabled
    self.decodedBGRA = decoded.bgra
    self.hashPrefix = "macos-first-frame"
    super.init()
    createNativeMetalUploader()
    rebuildPixelBuffer()
  }

  deinit {
    if let nativeMetalUploader {
      VPMacOSMetalUploaderDestroy(nativeMetalUploader)
    }
  }

  func resize(width: Int, height: Int) {
    lock.lock()
    defer { lock.unlock() }

    guard syntheticPattern else { return }
    guard width != self.width || height != self.height else { return }
    self.width = width
    self.height = height
    rebuildPixelBufferLocked()
  }

  func update(decoded: MacOSDecodedFirstFrame) {
    lock.lock()
    defer { lock.unlock() }

    guard !syntheticPattern else { return }
    let sizeChanged = width != decoded.width || height != decoded.height
    width = decoded.width
    height = decoded.height
    decodedBGRA = decoded.bgra
    if sizeChanged || pixelBuffer == nil {
      rebuildPixelBufferLocked()
    } else if let pixelBuffer {
      copyBGRA(decoded.bgra, to: pixelBuffer)
      validateMetalTextureLocked(buffer: pixelBuffer)
      pixelBufferReuseCount += 1
    }
  }

  func updateFromNativePlayer(
    _ player: MacOSNativePlayerSession,
    waitTimeoutMs: Int
  ) throws -> MacOSNativeFrameInfo {
    lock.lock()
    defer { lock.unlock() }

    guard !syntheticPattern else {
      throw MacOSNativePlayerError.invalidPayload
    }
    if pixelBuffer == nil {
      rebuildPixelBufferLocked()
    }
    guard let pixelBuffer else {
      throw MacOSNativePlayerError.invalidPayload
    }

    if let info = try copyFromNativePlayerWithMetalUpload(
      player,
      pixelBuffer: pixelBuffer,
      waitTimeoutMs: waitTimeoutMs
    ) {
      pixelBufferReuseCount += 1
      return info
    }

    CVPixelBufferLockBaseAddress(pixelBuffer, [])
    guard let baseAddress = CVPixelBufferGetBaseAddress(pixelBuffer) else {
      CVPixelBufferUnlockBaseAddress(pixelBuffer, [])
      throw MacOSNativePlayerError.invalidPayload
    }
    let bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
    let dstSize = bytesPerRow * height
    let info: MacOSNativeFrameInfo
    do {
      info = try player.copyCurrentFrameIntoBGRA(
        baseAddress.assumingMemoryBound(to: UInt8.self),
        dstSize: dstSize,
        width: width,
        height: height,
        strideBytes: bytesPerRow,
        waitTimeoutMs: waitTimeoutMs
      )
    } catch {
      CVPixelBufferUnlockBaseAddress(pixelBuffer, [])
      throw error
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, [])
    validateMetalTextureLocked(buffer: pixelBuffer)
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
    metalUploadFailureCount: Int,
    metalUploadEnabled: Bool,
    presentationUploadMode: String,
    metalAvailable: Bool,
    metalTextureCacheAvailable: Bool,
    metalTextureValid: Bool,
    metalTextureCreationCount: Int,
    metalTextureFailureCount: Int
  ) {
    lock.lock()
    defer { lock.unlock() }

    return (
      rebuildCount: pixelBufferRebuildCount,
      reuseCount: pixelBufferReuseCount,
      directCopyCount: pixelBufferDirectCopyCount,
      metalUploadCount: pixelBufferMetalUploadCount,
      metalUploadFailureCount: pixelBufferMetalUploadFailureCount,
      metalUploadEnabled: metalUploadEnabled,
      presentationUploadMode: presentationUploadModeLocked(),
      metalAvailable: nativeMetalUploaderAvailableLocked(),
      metalTextureCacheAvailable: nativeMetalUploaderAvailableLocked(),
      metalTextureValid: metalTextureValid,
      metalTextureCreationCount: metalTextureCreationCount,
      metalTextureFailureCount: metalTextureFailureCount
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

    var nextBuffer: CVPixelBuffer?
    let status = CVPixelBufferCreate(
      kCFAllocatorDefault,
      width,
      height,
      kCVPixelFormatType_32BGRA,
      attributes,
      &nextBuffer
    )
    guard status == kCVReturnSuccess, let nextBuffer else {
      pixelBuffer = nil
      return
    }
    pixelBufferRebuildCount += 1

    if let decodedBGRA {
      copyBGRA(decodedBGRA, to: nextBuffer)
    } else {
      fill(buffer: nextBuffer)
    }
    pixelBuffer = nextBuffer
    validateMetalTextureLocked(buffer: nextBuffer)
  }

  private func createNativeMetalUploader() {
    nativeMetalUploader = VPMacOSMetalUploaderCreate()
  }

  private func nativeMetalUploaderAvailableLocked() -> Bool {
    guard let nativeMetalUploader else { return false }
    return VPMacOSMetalUploaderIsAvailable(nativeMetalUploader) != 0
  }

  private func presentationUploadModeLocked() -> String {
    if metalUploadEnabled, metalTextureValid, nativeMetalUploaderAvailableLocked() {
      return "metal-bgra-staging-upload"
    }
    if pixelBuffer != nil {
      return "cvpixelbuffer-direct-copy"
    }
    return "unavailable"
  }

  private func copyFromNativePlayerWithMetalUpload(
    _ player: MacOSNativePlayerSession,
    pixelBuffer: CVPixelBuffer,
    waitTimeoutMs: Int
  ) throws -> MacOSNativeFrameInfo? {
    guard metalUploadEnabled else {
      return nil
    }
    guard let nativeMetalUploader,
          VPMacOSMetalUploaderIsAvailable(nativeMetalUploader) != 0 else {
      return nil
    }
    do {
      guard let info = try player.copyCurrentFrameToMetalPixelBuffer(
        uploader: nativeMetalUploader,
        pixelBuffer: pixelBuffer,
        width: width,
        height: height,
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
      throw error
    }
  }

  private func validateMetalTextureLocked(buffer: CVPixelBuffer) {
    guard let nativeMetalUploader else {
      metalTextureValid = false
      return
    }

    if VPMacOSMetalUploaderValidatePixelBuffer(
      nativeMetalUploader,
      UnsafeMutableRawPointer(Unmanaged.passUnretained(buffer).toOpaque()),
      Int32(width),
      Int32(height)
    ) != 0 {
      metalTextureCreationCount += 1
      metalTextureValid = true
    } else {
      metalTextureFailureCount += 1
      metalTextureValid = false
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
